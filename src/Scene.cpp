#include "Scene.h"
#include "Config.h"
#include "Renderer.h"
#include "CommonResources.h"
#include "Utilities.h"

// Enable ForwardLighting shared definitions for C++ side
#define FORWARD_LIGHTING_DEFINE
#include "shaders/ShaderShared.hlsl"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

// stb_image for loading textures
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include "../external/stb_image.h"

// CPU vertex layout used for uploading
struct Vertex
{
	Vector3 pos;
	Vector3 normal;
	Vector2 uv;
};

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
static void ProcessMaterialsAndImages(cgltf_data* data, Scene& scene)
{
	SCOPED_TIMER("[Scene] Materials+Images");

	// Materials
	for (cgltf_size i = 0; i < data->materials_count; ++i)
	{
		scene.m_Materials.emplace_back();
		scene.m_Materials.back().m_Name = data->materials[i].name ? data->materials[i].name : std::string();
		const cgltf_pbr_metallic_roughness& pbr = data->materials[i].pbr_metallic_roughness;
		scene.m_Materials.back().m_BaseColorFactor.x = pbr.base_color_factor[0];
		scene.m_Materials.back().m_BaseColorFactor.y = pbr.base_color_factor[1];
		scene.m_Materials.back().m_BaseColorFactor.z = pbr.base_color_factor[2];
		scene.m_Materials.back().m_BaseColorFactor.w = pbr.base_color_factor[3];
		if (pbr.base_color_texture.texture && pbr.base_color_texture.texture->image)
		{
			cgltf_size imgIndex = pbr.base_color_texture.texture->image - data->images;
			scene.m_Materials.back().m_BaseColorTexture = static_cast<int>(imgIndex);
		}

		float metallic = pbr.metallic_factor;
		if (pbr.metallic_roughness_texture.texture == NULL && metallic == 1.0f)
			metallic = 0.0f;
		scene.m_Materials.back().m_RoughnessFactor = pbr.roughness_factor;
		scene.m_Materials.back().m_MetallicFactor = metallic;

		if (pbr.metallic_roughness_texture.texture && pbr.metallic_roughness_texture.texture->image)
		{
			cgltf_size imgIndex = pbr.metallic_roughness_texture.texture->image - data->images;
			scene.m_Materials.back().m_MetallicRoughnessTexture = static_cast<int>(imgIndex);
		}

		if (data->materials[i].normal_texture.texture && data->materials[i].normal_texture.texture->image)
		{
			cgltf_size imgIndex = data->materials[i].normal_texture.texture->image - data->images;
			scene.m_Materials.back().m_NormalTexture = static_cast<int>(imgIndex);
		}
	}

	// Images -> textures (URI only)
	for (cgltf_size i = 0; i < data->images_count; ++i)
	{
		scene.m_Textures.emplace_back();
		scene.m_Textures.back().m_Uri = data->images[i].uri ? data->images[i].uri : std::string();
	}
}

static void LoadTexturesFromImages(Scene& scene, cgltf_data* data, const std::filesystem::path& sceneDir, Renderer* renderer)
{
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

		int width = 0, height = 0, channels = 0;
		unsigned char* imgData = stbi_load(fullPath.c_str(), &width, &height, &channels, 4);
		if (!imgData)
		{
			SDL_LOG_ASSERT_FAIL("Texture load failed", "[Scene] Failed to load texture: %s", fullPath.c_str());
			continue;
		}

		nvrhi::TextureDesc desc;
		desc.width = width;
		desc.height = height;
		desc.format = nvrhi::Format::RGBA8_UNORM;
		desc.isShaderResource = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = tex.m_Uri.c_str();
		tex.m_Handle = renderer->m_NvrhiDevice->createTexture(desc);
		if (!tex.m_Handle)
		{
			SDL_LOG_ASSERT_FAIL("Texture creation failed", "[Scene] GPU texture creation failed for %s", tex.m_Uri.c_str());
			stbi_image_free(imgData);
			continue;
		}

		nvrhi::CommandListHandle cmd = renderer->AcquireCommandList("Upload Texture");
		const size_t bytesPerPixel = nvrhi::getFormatInfo(desc.format).bytesPerBlock;
		const size_t rowPitch = (size_t)width * bytesPerPixel;
		const size_t depthPitch = rowPitch * (size_t)height;
		cmd->writeTexture(tex.m_Handle, 0, 0, imgData, rowPitch, depthPitch);
		renderer->SubmitCommandList(cmd);

		tex.m_BindlessIndex = renderer->RegisterTexture(tex.m_Handle);
		if (tex.m_BindlessIndex == UINT32_MAX)
		{
			SDL_LOG_ASSERT_FAIL("Bindless texture registration failed", "[Scene] Bindless texture registration failed for %s", tex.m_Uri.c_str());
		}

		stbi_image_free(imgData);
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

		nvrhi::CommandListHandle cmd = renderer->AcquireCommandList("Upload MaterialConstants");
		cmd->writeBuffer(scene.m_MaterialConstantsBuffer, materialConstants.data(), materialConstants.size() * sizeof(MaterialConstants));
		renderer->SubmitCommandList(cmd);
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

			const cgltf_size vertCount = posAcc->count;
			p.m_VertexOffset = static_cast<uint32_t>(outVertices.size());
			p.m_VertexCount = static_cast<uint32_t>(vertCount);

			for (cgltf_size v = 0; v < vertCount; ++v)
			{
				Vertex vx{};
				float pos[4] = { 0,0,0,0 };
				cgltf_size posComps = cgltf_num_components(posAcc->type);
				cgltf_accessor_read_float(posAcc, v, pos, posComps);
				vx.pos.x = pos[0]; vx.pos.y = pos[1]; vx.pos.z = pos[2];

				float nrm[4] = { 0,0,0,0 };
				if (normAcc)
				{
					cgltf_size nrmComps = cgltf_num_components(normAcc->type);
					cgltf_accessor_read_float(normAcc, v, nrm, nrmComps);
				}
				vx.normal.x = nrm[0]; vx.normal.y = nrm[1]; vx.normal.z = nrm[2];

				float uv[4] = { 0,0,0,0 };
				if (uvAcc)
				{
					cgltf_size uvComps = cgltf_num_components(uvAcc->type);
					cgltf_accessor_read_float(uvAcc, v, uv, uvComps);
				}
				vx.uv.x = uv[0]; vx.uv.y = uv[1];

				ExtendAABB(mesh.m_AabbMin, mesh.m_AabbMax, vx.pos);
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

			p.m_MaterialIndex = prim.material ? static_cast<int>(prim.material - data->materials) : -1;
			mesh.m_Primitives.push_back(p);
		}

		scene.m_Meshes.push_back(std::move(mesh));
	}
}

static void ProcessNodesAndHierarchy(cgltf_data* data, Scene& scene)
{
	SCOPED_TIMER("[Scene] Nodes+Hierarchy");
	std::unordered_map<const cgltf_node*, int> nodeMap;
	for (cgltf_size ni = 0; ni < data->nodes_count; ++ni)
	{
		const cgltf_node& cn = data->nodes[ni];
		Scene::Node node;
		node.m_Name = cn.name ? cn.name : std::string();
		node.m_MeshIndex = cn.mesh ? static_cast<int>(cn.mesh - data->meshes) : -1;
		node.m_CameraIndex = cn.camera ? static_cast<int>(cn.camera - data->cameras) : -1;
		node.m_LightIndex = cn.light ? static_cast<int>(cn.light - data->lights) : -1;

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
		nodeMap[&cn] = static_cast<int>(scene.m_Nodes.size()) - 1;
	}

	// Build parent/children links
	for (cgltf_size ni = 0; ni < data->nodes_count; ++ni)
	{
		const cgltf_node& cn = data->nodes[ni];
		int idx = nodeMap.at(&cn);
		if (cn.children_count > 0)
		{
			for (cgltf_size ci = 0; ci < cn.children_count; ++ci)
			{
				const cgltf_node* child = cn.children[ci];
				int childIdx = nodeMap.at(child);
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
			node.m_AabbMin = Vector3{ FLT_MAX, FLT_MAX, FLT_MAX };
			node.m_AabbMax = Vector3{ -FLT_MAX, -FLT_MAX, -FLT_MAX };

			DirectX::XMMATRIX world = DirectX::XMLoadFloat4x4(&node.m_WorldTransform);
			Vector3 corners[8];
			corners[0] = Vector3{ mesh.m_AabbMin.x, mesh.m_AabbMin.y, mesh.m_AabbMin.z };
			corners[1] = Vector3{ mesh.m_AabbMax.x, mesh.m_AabbMin.y, mesh.m_AabbMin.z };
			corners[2] = Vector3{ mesh.m_AabbMin.x, mesh.m_AabbMax.y, mesh.m_AabbMin.z };
			corners[3] = Vector3{ mesh.m_AabbMax.x, mesh.m_AabbMax.y, mesh.m_AabbMin.z };
			corners[4] = Vector3{ mesh.m_AabbMin.x, mesh.m_AabbMin.y, mesh.m_AabbMax.z };
			corners[5] = Vector3{ mesh.m_AabbMax.x, mesh.m_AabbMin.y, mesh.m_AabbMax.z };
			corners[6] = Vector3{ mesh.m_AabbMin.x, mesh.m_AabbMax.y, mesh.m_AabbMax.z };
			corners[7] = Vector3{ mesh.m_AabbMax.x, mesh.m_AabbMax.y, mesh.m_AabbMax.z };

			for (int c = 0; c < 8; ++c)
			{
				Vector v = DirectX::XMLoadFloat3(reinterpret_cast<const Vector3*>(&corners[c]));
				Vector vt = DirectX::XMVector3Transform(v, world);
				Vector3 vt3;
				DirectX::XMStoreFloat3(&vt3, vt);
				Vector3 tv{ vt3.x, vt3.y, vt3.z };
				ExtendAABB(node.m_AabbMin, node.m_AabbMax, tv);
			}
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
			renderer->m_DirectionalLight.yaw = yaw;
			renderer->m_DirectionalLight.pitch = pitch;
			renderer->m_DirectionalLight.intensity = light.m_Intensity * 10000.0f;
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
		desc.isVertexBuffer = true;
		desc.initialState = nvrhi::ResourceStates::VertexBuffer;
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
		nvrhi::CommandListHandle cmd = renderer->AcquireCommandList("Upload Scene");
		if (scene.m_VertexBuffer && vbytes > 0)
			cmd->writeBuffer(scene.m_VertexBuffer, allVertices.data(), vbytes, 0);
		if (scene.m_IndexBuffer && ibytes > 0)
			cmd->writeBuffer(scene.m_IndexBuffer, allIndices.data(), ibytes, 0);

		renderer->SubmitCommandList(cmd);
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
		SDL_LOG_ASSERT_FAIL("glTF parse failed", "[Scene] Failed to parse glTF file: %s", scenePath.c_str());
		return false;
	}

	res = cgltf_load_buffers(&options, data, scenePath.c_str());
	if (res != cgltf_result_success)
	{
		SDL_LOG_ASSERT_FAIL("glTF buffer load failed", "[Scene] Failed to load glTF buffers");
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

	// Clear CPU-side containers
	m_Meshes.clear();
	m_Nodes.clear();
	m_Materials.clear();
	for (auto& tex : m_Textures)
	{
		tex.m_Handle = nullptr;
	}
	m_Textures.clear();
	m_Cameras.clear();
	m_Lights.clear();
}
