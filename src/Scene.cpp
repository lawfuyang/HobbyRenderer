#include "Scene.h"
#include "Config.h"
#include "Renderer.h"
#include "CommonResources.h"
#include "Utilities.h"
#include "TextureLoader.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include "meshoptimizer.h"

const char* cgltf_result_tostring(cgltf_result result)
{
	switch (result)
	{
	case cgltf_result_success: return "success";
	case cgltf_result_data_too_short: return "data_too_short";
	case cgltf_result_unknown_format: return "unknown_format";
	case cgltf_result_invalid_json: return "invalid_json";
	case cgltf_result_invalid_gltf: return "invalid_gltf";
	case cgltf_result_invalid_options: return "invalid_options";
	case cgltf_result_file_not_found: return "file_not_found";
	case cgltf_result_io_error: return "io_error";
	case cgltf_result_out_of_memory: return "out_of_memory";
	case cgltf_result_legacy_gltf: return "legacy_gltf";
	default: return "unknown";
	}
}

// referred from meshoptimizer
static cgltf_result decompressMeshopt(cgltf_data* data)
{
	for (size_t i = 0; i < data->buffer_views_count; ++i)
	{
		if (!data->buffer_views[i].has_meshopt_compression)
			continue;
		cgltf_meshopt_compression* mc = &data->buffer_views[i].meshopt_compression;

		const unsigned char* source = (const unsigned char*)mc->buffer->data;
		if (!source)
			return cgltf_result_invalid_gltf;
		source += mc->offset;

		void* result = malloc(mc->count * mc->stride);
		if (!result)
			return cgltf_result_out_of_memory;

		data->buffer_views[i].data = result;

		int rc = -1;

		switch (mc->mode)
		{
		case cgltf_meshopt_compression_mode_attributes:
			rc = meshopt_decodeVertexBuffer(result, mc->count, mc->stride, source, mc->size);
			break;

		case cgltf_meshopt_compression_mode_triangles:
			rc = meshopt_decodeIndexBuffer(result, mc->count, mc->stride, source, mc->size);
			break;

		case cgltf_meshopt_compression_mode_indices:
			rc = meshopt_decodeIndexSequence(result, mc->count, mc->stride, source, mc->size);
			break;

		default:
			return cgltf_result_invalid_gltf;
		}

		if (rc != 0)
			return cgltf_result_io_error;

		switch (mc->filter)
		{
		case cgltf_meshopt_compression_filter_octahedral:
			meshopt_decodeFilterOct(result, mc->count, mc->stride);
			break;

		case cgltf_meshopt_compression_filter_quaternion:
			meshopt_decodeFilterQuat(result, mc->count, mc->stride);
			break;

		case cgltf_meshopt_compression_filter_exponential:
			meshopt_decodeFilterExp(result, mc->count, mc->stride);
			break;

		// uncomment when cgltf version has this filter
	#if 0
		case cgltf_meshopt_compression_filter_color:
			meshopt_decodeFilterColor(result, mc->count, mc->stride);
			break;
	#endif

		default:
			break;
		}
	}

	return cgltf_result_success;
}

// Helpers for AABB
static void ExtendAABB(Vector3& minOut, Vector3& maxOut, const Vector3& v)
{
	if (v.x < minOut.x) minOut.x = v.x;
	if (v.y < minOut.y) minOut.y = v.y;
	if (v.z < minOut.z) minOut.z = v.z;
	if (v.x > maxOut.x) maxOut.x = v.x;
	if (v.y > maxOut.y) maxOut.y = v.y;
	if (v.z > maxOut.z) maxOut.z = v.z;
}

// Recursively compute world transforms for a Scene instance
static void ComputeWorldTransforms(Scene& scene, int nodeIndex, const Matrix& parent)
{
	Scene::Node& node = scene.m_Nodes[nodeIndex];
	DirectX::XMMATRIX localM = DirectX::XMLoadFloat4x4(&node.m_LocalTransform);
	DirectX::XMMATRIX parentM = DirectX::XMLoadFloat4x4(&parent);
	DirectX::XMMATRIX worldM = DirectX::XMMatrixMultiply(localM, parentM);
	Matrix worldOut{};
	DirectX::XMStoreFloat4x4(&worldOut, worldM);
	node.m_WorldTransform = worldOut;

	for (int child : node.m_Children)
		ComputeWorldTransforms(scene, child, node.m_WorldTransform);
}

// --- Helper pieces extracted from Scene::LoadScene for clarity ---
static void SetTextureAndSampler(const cgltf_texture* tex, int& textureIndex, std::vector<bool>& samplerForImageIsWrap, const cgltf_data* data)
{
    if (tex && tex->image)
    {
        cgltf_size imgIndex = cgltf_image_index(data, tex->image);
        if (!Config::Get().m_SkipTextures)
        {
            textureIndex = static_cast<int>(imgIndex);
        }
        if (tex->sampler)
        {
            cgltf_sampler* s = tex->sampler;
            bool isWrap = (s->wrap_s == cgltf_wrap_mode_repeat || s->wrap_t == cgltf_wrap_mode_repeat);
            samplerForImageIsWrap[imgIndex] = isWrap;
        }
    }
}

static void ProcessMaterialsAndImages(cgltf_data* data, Scene& scene)
{
	SCOPED_TIMER("[Scene] Materials+Images");

	// Prepare per-image sampler mapping (default unknown)
	std::vector<bool> samplerForImageIsWrap(data->images_count, false);

	// Materials
	for (cgltf_size i = 0; i < data->materials_count; ++i)
	{
		scene.m_Materials.emplace_back();
		scene.m_Materials.back().m_Name = data->materials[i].name ? data->materials[i].name : std::string();
		const cgltf_pbr_metallic_roughness& pbr = data->materials[i].pbr_metallic_roughness;
		if (data->materials[i].has_pbr_specular_glossiness)
		{
			const cgltf_pbr_specular_glossiness& sg = data->materials[i].pbr_specular_glossiness;
			// Convert specular-glossiness to metallic-roughness
			scene.m_Materials.back().m_BaseColorFactor.x = sg.diffuse_factor[0];
			scene.m_Materials.back().m_BaseColorFactor.y = sg.diffuse_factor[1];
			scene.m_Materials.back().m_BaseColorFactor.z = sg.diffuse_factor[2];
			scene.m_Materials.back().m_BaseColorFactor.w = sg.diffuse_factor[3];
			// Roughness = 1 - glossiness
			scene.m_Materials.back().m_RoughnessFactor = 1.0f - sg.glossiness_factor;
			// Metallic = dielectric approximation
			scene.m_Materials.back().m_MetallicFactor = std::max(std::max(sg.specular_factor[0], sg.specular_factor[1]), sg.specular_factor[2]);

			SetTextureAndSampler(sg.diffuse_texture.texture, scene.m_Materials.back().m_BaseColorTexture, samplerForImageIsWrap, data);
			SetTextureAndSampler(sg.specular_glossiness_texture.texture, scene.m_Materials.back().m_MetallicRoughnessTexture, samplerForImageIsWrap, data);
		}
		else
		{
			scene.m_Materials.back().m_BaseColorFactor.x = pbr.base_color_factor[0];
			scene.m_Materials.back().m_BaseColorFactor.y = pbr.base_color_factor[1];
			scene.m_Materials.back().m_BaseColorFactor.z = pbr.base_color_factor[2];
			scene.m_Materials.back().m_BaseColorFactor.w = pbr.base_color_factor[3];
			SetTextureAndSampler(pbr.base_color_texture.texture, scene.m_Materials.back().m_BaseColorTexture, samplerForImageIsWrap, data);

			float metallic = pbr.metallic_factor;
			if (pbr.metallic_roughness_texture.texture == NULL && metallic == 1.0f)
				metallic = 0.0f;
			scene.m_Materials.back().m_RoughnessFactor = pbr.roughness_factor;
			scene.m_Materials.back().m_MetallicFactor = metallic;

			SetTextureAndSampler(pbr.metallic_roughness_texture.texture, scene.m_Materials.back().m_MetallicRoughnessTexture, samplerForImageIsWrap, data);
		}

		SetTextureAndSampler(data->materials[i].normal_texture.texture, scene.m_Materials.back().m_NormalTexture, samplerForImageIsWrap, data);
	}

	// Images -> textures (URI only)
	for (cgltf_size i = 0; i < data->images_count; ++i)
	{
		scene.m_Textures.emplace_back();
		scene.m_Textures.back().m_Uri = data->images[i].uri ? data->images[i].uri : std::string();
		scene.m_Textures.back().m_Sampler = samplerForImageIsWrap[i] ? Scene::Texture::Wrap : Scene::Texture::Clamp;
	}
}

static void LoadTexturesFromImages(Scene& scene, cgltf_data* data, const std::filesystem::path& sceneDir, Renderer* renderer)
{
	if (Config::Get().m_SkipTextures)
	{
		return;
	}

	SCOPED_TIMER("[Scene] LoadTextures");

	for (size_t ti = 0; ti < scene.m_Textures.size(); ++ti)
	{
		auto& tex = scene.m_Textures[ti];
		if (tex.m_Uri.empty())
		{
			SDL_Log("[Scene] Texture %zu has no URI, skipping (embedded images not yet supported)", ti);
			continue;
		}

		std::string fullPath = (sceneDir / tex.m_Uri).string();
		cgltf_decode_uri(fullPath.data());

		if (!std::filesystem::exists(fullPath))
		{
			SDL_LOG_ASSERT_FAIL("Texture file not found", "[Scene] Texture file not found: %s", fullPath.c_str());
			continue;
		}

		nvrhi::TextureDesc desc;
		std::vector<uint8_t> imgData;
		LoadSTBITexture(fullPath, desc, imgData);
		if (imgData.empty())
		{
			SDL_LOG_ASSERT_FAIL("Texture loading failed", "[Scene] Texture loading failed for %s", tex.m_Uri.c_str());
			continue;
		}

		desc.isShaderResource = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = tex.m_Uri.c_str();
		tex.m_Handle = renderer->m_NvrhiDevice->createTexture(desc);
		if (!tex.m_Handle)
		{
			SDL_LOG_ASSERT_FAIL("Texture creation failed", "[Scene] GPU texture creation failed for %s", tex.m_Uri.c_str());
			continue;
		}

		ScopedCommandList cmd{ "Upload Texture" };
		const size_t bytesPerPixel = nvrhi::getFormatInfo(desc.format).bytesPerBlock;
		const size_t rowPitch = (size_t)desc.width * bytesPerPixel;
		const size_t depthPitch = rowPitch * (size_t)desc.height;
		cmd->writeTexture(tex.m_Handle, 0, 0, imgData.data(), rowPitch, depthPitch);

		tex.m_BindlessIndex = renderer->RegisterTexture(tex.m_Handle);
		if (tex.m_BindlessIndex == UINT32_MAX)
		{
			SDL_LOG_ASSERT_FAIL("Bindless texture registration failed", "[Scene] Bindless texture registration failed for %s", tex.m_Uri.c_str());
		}
	}
}

static void UpdateMaterialsAndCreateConstants(Scene& scene, Renderer* renderer)
{
	SCOPED_TIMER("[Scene] MaterialConstants");

	for (auto& mat : scene.m_Materials)
	{
		if (mat.m_BaseColorTexture != -1)
			mat.m_AlbedoTextureIndex = scene.m_Textures[mat.m_BaseColorTexture].m_BindlessIndex;
		if (mat.m_NormalTexture != -1)
			mat.m_NormalTextureIndex = scene.m_Textures[mat.m_NormalTexture].m_BindlessIndex;
		if (mat.m_MetallicRoughnessTexture != -1)
			mat.m_RoughnessMetallicTextureIndex = scene.m_Textures[mat.m_MetallicRoughnessTexture].m_BindlessIndex;
	}

	std::vector<MaterialConstants> materialConstants;
	materialConstants.reserve(scene.m_Materials.size());
	for (const auto& mat : scene.m_Materials)
	{
		MaterialConstants mc{};
		mc.m_BaseColor = mat.m_BaseColorFactor;
		mc.m_RoughnessMetallic = Vector2{ mat.m_RoughnessFactor, mat.m_MetallicFactor };
		mc.m_TextureFlags = 0;
		if (mat.m_BaseColorTexture != -1) mc.m_TextureFlags |= TEXFLAG_ALBEDO;
		if (mat.m_NormalTexture != -1) mc.m_TextureFlags |= TEXFLAG_NORMAL;
		if (mat.m_MetallicRoughnessTexture != -1) mc.m_TextureFlags |= TEXFLAG_ROUGHNESS_METALLIC;
		mc.m_AlbedoTextureIndex = mat.m_AlbedoTextureIndex;
		mc.m_NormalTextureIndex = mat.m_NormalTextureIndex;
		mc.m_RoughnessMetallicTextureIndex = mat.m_RoughnessMetallicTextureIndex;
		// Per-texture sampler indices (do not assume they are the same)
		if (mat.m_BaseColorTexture != -1)
			mc.m_AlbedoSamplerIndex = (uint32_t)scene.m_Textures[mat.m_BaseColorTexture].m_Sampler;
		else
			mc.m_AlbedoSamplerIndex = (uint32_t)Scene::Texture::Wrap;

		if (mat.m_NormalTexture != -1)
			mc.m_NormalSamplerIndex = (uint32_t)scene.m_Textures[mat.m_NormalTexture].m_Sampler;
		else
			mc.m_NormalSamplerIndex = (uint32_t)Scene::Texture::Wrap;

		if (mat.m_MetallicRoughnessTexture != -1)
			mc.m_RoughnessSamplerIndex = (uint32_t)scene.m_Textures[mat.m_MetallicRoughnessTexture].m_Sampler;
		else
			mc.m_RoughnessSamplerIndex = (uint32_t)Scene::Texture::Wrap;
		materialConstants.push_back(mc);
	}

	if (!materialConstants.empty())
	{
		nvrhi::BufferDesc matBufDesc = nvrhi::BufferDesc()
			.setByteSize(materialConstants.size() * sizeof(MaterialConstants))
			.setStructStride(sizeof(MaterialConstants))
			.setInitialState(nvrhi::ResourceStates::ShaderResource)
			.setKeepInitialState(true);
		scene.m_MaterialConstantsBuffer = renderer->m_NvrhiDevice->createBuffer(matBufDesc);
		renderer->m_RHI.SetDebugName(scene.m_MaterialConstantsBuffer, "MaterialConstantsBuffer");

		ScopedCommandList cmd{ "Upload MaterialConstants" };
		cmd->writeBuffer(scene.m_MaterialConstantsBuffer, materialConstants.data(), materialConstants.size() * sizeof(MaterialConstants));
	}
}

static void ProcessCameras(cgltf_data* data, Scene& scene)
{
	SCOPED_TIMER("[Scene] Cameras");
	for (cgltf_size i = 0; i < data->cameras_count; ++i)
	{
		const cgltf_camera& cgCam = data->cameras[i];
		Scene::Camera cam;
		cam.m_Name = cgCam.name ? cgCam.name : std::string();
		if (cgCam.type == cgltf_camera_type_perspective)
		{
			const auto& p = cgCam.data.perspective;
			cam.m_Projection.aspectRatio = p.has_aspect_ratio ? p.aspect_ratio : (16.0f / 9.0f);
			cam.m_Projection.fovY = p.yfov;
			cam.m_Projection.nearZ = p.znear;
			scene.m_Cameras.push_back(std::move(cam));
		}
		else if (cgCam.type == cgltf_camera_type_orthographic)
		{
			SDL_Log("[Scene] Skipping orthographic camera: %s", cam.m_Name.c_str());
		}
		else
		{
			SDL_Log("[Scene] Unknown camera type for camera: %s", cam.m_Name.c_str());
		}
	}
}

static void ProcessLights(cgltf_data* data, Scene& scene)
{
	SCOPED_TIMER("[Scene] Lights");
	for (cgltf_size i = 0; i < data->lights_count; ++i)
	{
		const cgltf_light& cgLight = data->lights[i];
		Scene::Light light;
		light.m_Name = cgLight.name ? cgLight.name : std::string();
		light.m_Color = Vector3{ cgLight.color[0], cgLight.color[1], cgLight.color[2] };
		light.m_Intensity = cgLight.intensity;
		light.m_Range = cgLight.range;
		light.m_SpotInnerConeAngle = cgLight.spot_inner_cone_angle;
		light.m_SpotOuterConeAngle = cgLight.spot_outer_cone_angle;
		if (cgLight.type == cgltf_light_type_directional)
			light.m_Type = Scene::Light::Directional;
		else if (cgLight.type == cgltf_light_type_point)
			light.m_Type = Scene::Light::Point;
		else if (cgLight.type == cgltf_light_type_spot)
			light.m_Type = Scene::Light::Spot;
		scene.m_Lights.push_back(std::move(light));
	}
}

static void ProcessMeshes(cgltf_data* data, Scene& scene, std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices)
{
	SCOPED_TIMER("[Scene] Meshes");
	outVertices.clear();
	outIndices.clear();
	scene.m_MeshData.clear();

	for (cgltf_size mi = 0; mi < data->meshes_count; ++mi)
	{
		cgltf_mesh& cgMesh = data->meshes[mi];
		Scene::Mesh mesh;
		mesh.m_AabbMin = Vector3{ FLT_MAX, FLT_MAX, FLT_MAX };
		mesh.m_AabbMax = Vector3{ -FLT_MAX, -FLT_MAX, -FLT_MAX };

		for (cgltf_size pi = 0; pi < cgMesh.primitives_count; ++pi)
		{
			cgltf_primitive& prim = cgMesh.primitives[pi];
			Scene::Primitive p;

			const cgltf_accessor* posAcc = nullptr;
			const cgltf_accessor* normAcc = nullptr;
			const cgltf_accessor* uvAcc = nullptr;

			for (cgltf_size ai = 0; ai < prim.attributes_count; ++ai)
			{
				cgltf_attribute& attr = prim.attributes[ai];
				if (attr.type == cgltf_attribute_type_position)
					posAcc = attr.data;
				else if (attr.type == cgltf_attribute_type_normal)
					normAcc = attr.data;
				else if (attr.type == cgltf_attribute_type_texcoord)
					uvAcc = attr.data;
			}

			if (!posAcc)
			{
				SDL_LOG_ASSERT_FAIL("Primitive missing POSITION attribute", "[Scene] Primitive missing POSITION attribute. Is this normal?");
				continue;
			}

			bool useAccessorBounds = posAcc->has_min && posAcc->has_max;
			if (useAccessorBounds) {
				ExtendAABB(mesh.m_AabbMin, mesh.m_AabbMax, Vector3{posAcc->min[0], posAcc->min[1], posAcc->min[2]});
				ExtendAABB(mesh.m_AabbMin, mesh.m_AabbMax, Vector3{posAcc->max[0], posAcc->max[1], posAcc->max[2]});
			}

			const cgltf_size vertCount = posAcc->count;
			p.m_VertexOffset = static_cast<uint32_t>(outVertices.size());
			p.m_VertexCount = static_cast<uint32_t>(vertCount);

			for (cgltf_size v = 0; v < vertCount; ++v)
			{
				Vertex vx{};
				float pos[4] = { 0,0,0,0 };
				cgltf_size posComps = cgltf_num_components(posAcc->type);
				cgltf_accessor_read_float(posAcc, v, pos, posComps);
				vx.m_Pos.x = pos[0]; vx.m_Pos.y = pos[1]; vx.m_Pos.z = pos[2];

				float nrm[4] = { 0,0,0,0 };
				if (normAcc)
				{
					cgltf_size nrmComps = cgltf_num_components(normAcc->type);
					cgltf_accessor_read_float(normAcc, v, nrm, nrmComps);
				}
				vx.m_Normal.x = nrm[0]; vx.m_Normal.y = nrm[1]; vx.m_Normal.z = nrm[2];

				float uv[4] = { 0,0,0,0 };
				if (uvAcc)
				{
					cgltf_size uvComps = cgltf_num_components(uvAcc->type);
					cgltf_accessor_read_float(uvAcc, v, uv, uvComps);
				}
				vx.m_Uv.x = uv[0]; vx.m_Uv.y = uv[1];

				if (!useAccessorBounds) ExtendAABB(mesh.m_AabbMin, mesh.m_AabbMax, vx.m_Pos);
				outVertices.push_back(vx);
			}

			p.m_IndexOffset = static_cast<uint32_t>(outIndices.size());
			p.m_IndexCount = 0;
			if (prim.indices)
			{
				const cgltf_size idxCount = prim.indices->count;
				p.m_IndexCount = static_cast<uint32_t>(idxCount);
				for (cgltf_size k = 0; k < idxCount; ++k)
				{
					cgltf_size rawIdx = cgltf_accessor_read_index(prim.indices, k);
					uint32_t idx = static_cast<uint32_t>(rawIdx);
					outIndices.push_back(static_cast<uint32_t>(p.m_VertexOffset) + idx);
				}
			}

			p.m_MaterialIndex = prim.material ? static_cast<int>(cgltf_material_index(data, prim.material)) : -1;

			// Extract MeshData
			p.m_MeshDataIndex = (uint32_t)scene.m_MeshData.size();
			MeshData md;
			md.m_IndexOffset = p.m_IndexOffset;
			md.m_IndexCount = p.m_IndexCount;
			scene.m_MeshData.push_back(md);

			mesh.m_Primitives.push_back(p);
		}

		scene.m_Meshes.push_back(std::move(mesh));
	}
}

static void ProcessNodesAndHierarchy(cgltf_data* data, Scene& scene)
{
	SCOPED_TIMER("[Scene] Nodes+Hierarchy");
	std::unordered_map<cgltf_size, int> nodeMap;
	for (cgltf_size ni = 0; ni < data->nodes_count; ++ni)
	{
		const cgltf_node& cn = data->nodes[ni];
		Scene::Node node;
		node.m_Name = cn.name ? cn.name : std::string();
		node.m_MeshIndex = cn.mesh ? static_cast<int>(cgltf_mesh_index(data, cn.mesh)) : -1;
		node.m_CameraIndex = cn.camera ? static_cast<int>(cgltf_camera_index(data, cn.camera)) : -1;
		node.m_LightIndex = cn.light ? static_cast<int>(cgltf_light_index(data, cn.light)) : -1;

		Matrix localOut{};
		if (cn.has_matrix)
		{
			for (int i = 0; i < 16; ++i)
				reinterpret_cast<float*>(&localOut)[i] = cn.matrix[i];
		}
		else
		{
			Vector trans = DirectX::XMVectorSet(0, 0, 0, 0);
			Vector scale = DirectX::XMVectorSet(1, 1, 1, 0);
			Vector rot = DirectX::XMQuaternionIdentity();
			if (cn.has_translation)
				trans = DirectX::XMVectorSet(cn.translation[0], cn.translation[1], cn.translation[2], 0);
			if (cn.has_scale)
				scale = DirectX::XMVectorSet(cn.scale[0], cn.scale[1], cn.scale[2], 0);
			if (cn.has_rotation)
				rot = DirectX::XMVectorSet(cn.rotation[0], cn.rotation[1], cn.rotation[2], cn.rotation[3]);

			DirectX::XMMATRIX localM = DirectX::XMMatrixScalingFromVector(scale) * DirectX::XMMatrixRotationQuaternion(rot) * DirectX::XMMatrixTranslationFromVector(trans);
			DirectX::XMStoreFloat4x4(&localOut, localM);
		}

		node.m_LocalTransform = localOut;
		node.m_WorldTransform = node.m_LocalTransform;

		scene.m_Nodes.push_back(std::move(node));
		cgltf_size nodeIndex = cgltf_node_index(data, &cn);
		nodeMap[nodeIndex] = static_cast<int>(scene.m_Nodes.size()) - 1;
	}

	// Build parent/children links
	for (cgltf_size ni = 0; ni < data->nodes_count; ++ni)
	{
		const cgltf_node& cn = data->nodes[ni];
		cgltf_size nodeIndex = cgltf_node_index(data, &cn);
		int idx = nodeMap.at(nodeIndex);
		if (cn.children_count > 0)
		{
			for (cgltf_size ci = 0; ci < cn.children_count; ++ci)
			{
				const cgltf_node* child = cn.children[ci];
				cgltf_size childNodeIndex = cgltf_node_index(data, child);
				int childIdx = nodeMap.at(childNodeIndex);
				scene.m_Nodes[idx].m_Children.push_back(childIdx);
				scene.m_Nodes[childIdx].m_Parent = idx;
			}
		}
	}

	// Set node indices in cameras and lights
	for (size_t i = 0; i < scene.m_Nodes.size(); ++i)
	{
		const Scene::Node& node = scene.m_Nodes[i];
		if (node.m_CameraIndex >= 0 && node.m_CameraIndex < static_cast<int>(scene.m_Cameras.size()))
		{
			scene.m_Cameras[node.m_CameraIndex].m_NodeIndex = static_cast<int>(i);
		}
		if (node.m_LightIndex >= 0 && node.m_LightIndex < static_cast<int>(scene.m_Lights.size()))
		{
			scene.m_Lights[node.m_LightIndex].m_NodeIndex = static_cast<int>(i);
		}
	}

	// Compute world transforms
	for (size_t i = 0; i < scene.m_Nodes.size(); ++i)
	{
		if (scene.m_Nodes[i].m_Parent == -1)
		{
			Matrix identity{};
			DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
			ComputeWorldTransforms(scene, static_cast<int>(i), identity);
		}
	}

	// Compute per-node AABB by transforming mesh AABB into world space
	for (size_t ni = 0; ni < scene.m_Nodes.size(); ++ni)
	{
		Scene::Node& node = scene.m_Nodes[ni];
		if (node.m_MeshIndex >= 0 && node.m_MeshIndex < static_cast<int>(scene.m_Meshes.size()))
		{
			Scene::Mesh& mesh = scene.m_Meshes[node.m_MeshIndex];

			// Create local bounding box
			DirectX::BoundingBox localBox;
			localBox.Center = DirectX::XMFLOAT3(
				(mesh.m_AabbMin.x + mesh.m_AabbMax.x) * 0.5f,
				(mesh.m_AabbMin.y + mesh.m_AabbMax.y) * 0.5f,
				(mesh.m_AabbMin.z + mesh.m_AabbMax.z) * 0.5f
			);
			localBox.Extents = DirectX::XMFLOAT3(
				(mesh.m_AabbMax.x - mesh.m_AabbMin.x) * 0.5f,
				(mesh.m_AabbMax.y - mesh.m_AabbMin.y) * 0.5f,
				(mesh.m_AabbMax.z - mesh.m_AabbMin.z) * 0.5f
			);

			// Transform to world space
			DirectX::BoundingBox worldBox;
			localBox.Transform(worldBox, DirectX::XMLoadFloat4x4(&node.m_WorldTransform));

			// Set node AABB
			node.m_AabbMin = Vector3{
				worldBox.Center.x - worldBox.Extents.x,
				worldBox.Center.y - worldBox.Extents.y,
				worldBox.Center.z - worldBox.Extents.z
			};
			node.m_AabbMax = Vector3{
				worldBox.Center.x + worldBox.Extents.x,
				worldBox.Center.y + worldBox.Extents.y,
				worldBox.Center.z + worldBox.Extents.z
			};
		}
	}
}

static void SetupDirectionalLightAndCamera(Scene& scene, Renderer* renderer)
{
	SCOPED_TIMER("[Scene] Setup Lights+Camera");
	for (const auto& light : scene.m_Lights)
	{
		if (light.m_Type == Scene::Light::Directional && light.m_NodeIndex >= 0 && light.m_NodeIndex < static_cast<int>(scene.m_Nodes.size()))
		{
			const Matrix& worldTransform = scene.m_Nodes[light.m_NodeIndex].m_WorldTransform;
			DirectX::XMMATRIX m = DirectX::XMLoadFloat4x4(&worldTransform);
			DirectX::XMVECTOR localDir = DirectX::XMVectorSet(0, 0, -1, 0);
			DirectX::XMVECTOR worldDir = DirectX::XMVector3TransformNormal(localDir, m);
			DirectX::XMFLOAT3 dir;
			DirectX::XMStoreFloat3(&dir, DirectX::XMVector3Normalize(worldDir));
			float yaw = atan2f(dir.x, dir.z);
			float pitch = asinf(dir.y);
			scene.m_DirectionalLight.yaw = yaw;
			scene.m_DirectionalLight.pitch = pitch;
			scene.m_DirectionalLight.intensity = light.m_Intensity;
			break;
		}
	}

	if (!scene.m_Cameras.empty())
	{
		const Scene::Camera& firstCam = scene.m_Cameras[0];
		renderer->SetCameraFromSceneCamera(firstCam);
		renderer->m_SelectedCameraIndex = 0;
	}
}

static void CreateAndUploadGpuBuffers(Scene& scene, Renderer* renderer, const std::vector<Vertex>& allVertices, const std::vector<uint32_t>& allIndices)
{
	SCOPED_TIMER("[Scene] GPU Upload");

	const size_t vbytes = allVertices.size() * sizeof(Vertex);
	const size_t ibytes = allIndices.size() * sizeof(uint32_t);

	if (vbytes > 0)
	{
		nvrhi::BufferDesc desc{};
		desc.byteSize = (uint32_t)vbytes;
		desc.structStride = sizeof(Vertex);
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		scene.m_VertexBuffer = renderer->m_NvrhiDevice->createBuffer(desc);
		renderer->m_RHI.SetDebugName(scene.m_VertexBuffer, "Scene_VertexBuffer");
	}

	if (ibytes > 0)
	{
		nvrhi::BufferDesc desc{};
		desc.byteSize = (uint32_t)ibytes;
		desc.isIndexBuffer = true;
		desc.initialState = nvrhi::ResourceStates::IndexBuffer;
		desc.keepInitialState = true;
		scene.m_IndexBuffer = renderer->m_NvrhiDevice->createBuffer(desc);
		renderer->m_RHI.SetDebugName(scene.m_IndexBuffer, "Scene_IndexBuffer");
	}

	if (scene.m_VertexBuffer || scene.m_IndexBuffer)
	{
		ScopedCommandList cmd{ "Upload Scene" };
		if (scene.m_VertexBuffer && vbytes > 0)
			cmd->writeBuffer(scene.m_VertexBuffer, allVertices.data(), vbytes, 0);
		if (scene.m_IndexBuffer && ibytes > 0)
			cmd->writeBuffer(scene.m_IndexBuffer, allIndices.data(), ibytes, 0);
	}

	// Create mesh data buffer
	if (!scene.m_MeshData.empty())
	{
		nvrhi::BufferDesc desc{};
		desc.byteSize = (uint32_t)(scene.m_MeshData.size() * sizeof(MeshData));
		desc.structStride = sizeof(MeshData);
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		scene.m_MeshDataBuffer = renderer->m_NvrhiDevice->createBuffer(desc);
		renderer->m_RHI.SetDebugName(scene.m_MeshDataBuffer, "Scene_MeshDataBuffer");

		ScopedCommandList cmd{ "Upload Mesh Data" };
		cmd->writeBuffer(scene.m_MeshDataBuffer, scene.m_MeshData.data(), scene.m_MeshData.size() * sizeof(MeshData), 0);
	}

	// Create instance data buffer
	if (!scene.m_InstanceData.empty())
	{
		nvrhi::BufferDesc desc{};
		desc.byteSize = (uint32_t)(scene.m_InstanceData.size() * sizeof(PerInstanceData));
		desc.structStride = sizeof(PerInstanceData);
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		scene.m_InstanceDataBuffer = renderer->m_NvrhiDevice->createBuffer(desc);
		renderer->m_RHI.SetDebugName(scene.m_InstanceDataBuffer, "Scene_InstanceDataBuffer");
	}

	// Upload instance data
	if (scene.m_InstanceDataBuffer)
	{
		ScopedCommandList cmd{ "Upload Scene Data" };
		if (scene.m_InstanceDataBuffer && !scene.m_InstanceData.empty())
			cmd->writeBuffer(scene.m_InstanceDataBuffer, scene.m_InstanceData.data(), scene.m_InstanceData.size() * sizeof(PerInstanceData), 0);
	}
}

bool Scene::LoadScene()
{
	const std::string& scenePath = Config::Get().m_GltfScene;
	if (scenePath.empty())
	{
		SDL_Log("[Scene] No glTF scene configured, skipping load");
		return true;
	}

	SDL_Log("[Scene] Loading glTF scene: %s", scenePath.c_str());

	SCOPED_TIMER("[Scene] LoadScene Total");

	cgltf_options options{};
	cgltf_data* data = nullptr;
	cgltf_result res = cgltf_parse_file(&options, scenePath.c_str(), &data);
	if (res != cgltf_result_success || !data)
	{
		SDL_LOG_ASSERT_FAIL("glTF parse failed", "[Scene] Failed to parse glTF file: %s (result: %s)", scenePath.c_str(), cgltf_result_tostring(res));
		return false;
	}

	res = cgltf_validate(data);
	if (res != cgltf_result_success)
	{
		SDL_LOG_ASSERT_FAIL("glTF validation failed", "[Scene] glTF validation failed for file: %s (result: %s)", scenePath.c_str(), cgltf_result_tostring(res));
		cgltf_free(data);
		return false;
	}

	res = cgltf_load_buffers(&options, data, scenePath.c_str());
	if (res != cgltf_result_success)
	{
		SDL_LOG_ASSERT_FAIL("glTF buffer load failed", "[Scene] Failed to load glTF buffers (result: %s)", cgltf_result_tostring(res));
		cgltf_free(data);
		return false;
	}

	res = decompressMeshopt(data);
	if (res != cgltf_result_success)
	{
		SDL_LOG_ASSERT_FAIL("glTF meshopt decompression failed", "[Scene] Failed to decompress meshopt-compressed data (result: %s)", cgltf_result_tostring(res));
		cgltf_free(data);
		return false;
	}

	Renderer* renderer = Renderer::GetInstance();
	std::filesystem::path sceneDir = std::filesystem::path(scenePath).parent_path();

	ProcessMaterialsAndImages(data, *this);
	LoadTexturesFromImages(*this, data, sceneDir, renderer);
	UpdateMaterialsAndCreateConstants(*this, renderer);

	ProcessCameras(data, *this);
	ProcessLights(data, *this);

	std::vector<Vertex> allVertices;
	std::vector<uint32_t> allIndices;
	ProcessMeshes(data, *this, allVertices, allIndices);

	ProcessNodesAndHierarchy(data, *this);

	// Fill instance data
	m_InstanceData.clear();
	for (const auto& node : m_Nodes)
	{
		if (node.m_MeshIndex < 0) continue;
		const auto& mesh = m_Meshes[node.m_MeshIndex];
		for (const auto& prim : mesh.m_Primitives)
		{
			PerInstanceData inst{};
			inst.m_World = node.m_WorldTransform;
			inst.m_MaterialIndex = prim.m_MaterialIndex;
			inst.m_MeshDataIndex = prim.m_MeshDataIndex;
			// Use precomputed bounding AABB
			inst.m_Min = node.m_AabbMin;
			inst.m_Max = node.m_AabbMax;
			m_InstanceData.push_back(inst);
		}
	}

	SetupDirectionalLightAndCamera(*this, renderer);

	CreateAndUploadGpuBuffers(*this, renderer, allVertices, allIndices);

	cgltf_free(data);
	SDL_Log("[Scene] Loaded meshes: %zu, nodes: %zu", m_Meshes.size(), m_Nodes.size());

	return true;
}

void Scene::Shutdown()
{
	// Release GPU buffer handles so NVRHI can free underlying resources
	m_VertexBuffer = nullptr;
	m_IndexBuffer = nullptr;
	m_MaterialConstantsBuffer = nullptr;
	m_InstanceDataBuffer = nullptr;
	m_MeshDataBuffer = nullptr;

	// Clear CPU-side containers
	m_Meshes.clear();
	m_Nodes.clear();
	m_Materials.clear();
	m_MeshData.clear();
	for (auto& tex : m_Textures)
	{
		tex.m_Handle = nullptr;
	}
	m_Textures.clear();
	m_Cameras.clear();
	m_Lights.clear();
	m_InstanceData.clear();
}

Vector3 Scene::GetDirectionalLightDirection() const
{
    // Convert yaw and pitch to direction vector
    // Yaw: rotation around Y axis, Pitch: elevation from XZ plane
    float cosYaw = cos(m_DirectionalLight.yaw);
    float sinYaw = sin(m_DirectionalLight.yaw);
    float cosPitch = cos(m_DirectionalLight.pitch);
    float sinPitch = sin(m_DirectionalLight.pitch);
    
    // Direction from surface to light (pointing toward the light)
    return Vector3{ -sinYaw * cosPitch, -sinPitch, -cosYaw * cosPitch };
}
