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

// --- Binary Serialization Helpers ---
template<typename T>
static void WritePOD(std::ostream& os, const T& value)
{
	os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template<typename T>
static void ReadPOD(std::istream& is, T& value)
{
	is.read(reinterpret_cast<char*>(&value), sizeof(T));
}

static void WriteString(std::ostream& os, const std::string& str)
{
	size_t size = str.size();
	WritePOD(os, size);
	os.write(str.data(), size);
}

static void ReadString(std::istream& is, std::string& str)
{
	size_t size;
	ReadPOD(is, size);
	str.resize(size);
	is.read(&str[0], size);
}

template<typename T>
static void WriteVector(std::ostream& os, const std::vector<T>& vec)
{
	size_t size = vec.size();
	WritePOD(os, size);
	if (size > 0)
		os.write(reinterpret_cast<const char*>(vec.data()), size * sizeof(T));
}

template<typename T>
static void ReadVector(std::istream& is, std::vector<T>& vec)
{
	size_t size;
	ReadPOD(is, size);
	vec.resize(size);
	if (size > 0)
		is.read(reinterpret_cast<char*>(vec.data()), size * sizeof(T));
}

// Recursively compute world transforms for a Scene instance
static void ComputeWorldTransforms(Scene& scene, int nodeIndex, const Matrix& parent)
{
	Scene::Node& node = scene.m_Nodes[nodeIndex];
	const DirectX::XMMATRIX localM = DirectX::XMLoadFloat4x4(&node.m_LocalTransform);
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
        const cgltf_size imgIndex = cgltf_image_index(data, tex->image);
        if (!Config::Get().m_SkipTextures)
        {
            textureIndex = static_cast<int>(imgIndex);
        }
        if (tex->sampler)
        {
            cgltf_sampler* s = tex->sampler;
            const bool isWrap = (s->wrap_s == cgltf_wrap_mode_repeat || s->wrap_t == cgltf_wrap_mode_repeat);
            samplerForImageIsWrap[imgIndex] = isWrap;
        }
    }
}

static void ProcessMaterialsAndImages(const cgltf_data* data, Scene& scene)
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
		else if (data->materials[i].has_pbr_metallic_roughness)
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
		else
		{
			// Default values
			scene.m_Materials.back().m_BaseColorFactor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
			scene.m_Materials.back().m_MetallicFactor = 0.0f;
			scene.m_Materials.back().m_RoughnessFactor = 1.0f;
		}

		SetTextureAndSampler(data->materials[i].normal_texture.texture, scene.m_Materials.back().m_NormalTexture, samplerForImageIsWrap, data);
		SetTextureAndSampler(data->materials[i].emissive_texture.texture, scene.m_Materials.back().m_EmissiveTexture, samplerForImageIsWrap, data);
		scene.m_Materials.back().m_EmissiveFactor.x = data->materials[i].emissive_factor[0];
		scene.m_Materials.back().m_EmissiveFactor.y = data->materials[i].emissive_factor[1];
		scene.m_Materials.back().m_EmissiveFactor.z = data->materials[i].emissive_factor[2];

		if (data->materials[i].alpha_mode == cgltf_alpha_mode_mask)
		{
			scene.m_Materials.back().m_AlphaMode = ALPHA_MODE_MASK;
			scene.m_Materials.back().m_AlphaCutoff = data->materials[i].alpha_cutoff;
		}
		else if (data->materials[i].alpha_mode == cgltf_alpha_mode_blend)
		{
			scene.m_Materials.back().m_AlphaMode = ALPHA_MODE_BLEND;
		}
		else
		{
			scene.m_Materials.back().m_AlphaMode = ALPHA_MODE_OPAQUE;
		}

		if (data->materials[i].has_transmission)
		{
			scene.m_Materials.back().m_AlphaMode = ALPHA_MODE_BLEND;
			scene.m_Materials.back().m_BaseColorFactor.w = 1.0f - data->materials[i].transmission.transmission_factor;
		}

		if (data->materials[i].double_sided)
		{
			scene.m_Materials.back().m_AlphaMode = ALPHA_MODE_MASK;
		}
	}

	// Images -> textures (URI only)
	for (cgltf_size i = 0; i < data->images_count; ++i)
	{
		scene.m_Textures.emplace_back();
		scene.m_Textures.back().m_Uri = data->images[i].uri ? data->images[i].uri : std::string();
		scene.m_Textures.back().m_Sampler = samplerForImageIsWrap[i] ? Scene::Texture::Wrap : Scene::Texture::Clamp;
	}
}

static void LoadTexturesFromImages(Scene& scene, const std::filesystem::path& sceneDir, Renderer* renderer)
{
	if (Config::Get().m_SkipTextures)
	{
		return;
	}

	SCOPED_TIMER("[Scene] LoadTextures");

	const uint32_t threadCount = renderer->m_TaskScheduler->GetThreadCount();
	std::vector<ScopedCommandList> threadCommandLists(threadCount);

	renderer->m_TaskScheduler->ParallelFor(static_cast<uint32_t>(scene.m_Textures.size()), [&](uint32_t i, uint32_t threadIndex)
	{
		Scene::Texture& tex = scene.m_Textures[i];
		if (tex.m_Uri.empty())
		{
			SDL_LOG_ASSERT_FAIL("Texture URI missing", "[Scene] Texture %u has no URI, skipping (embedded images not yet supported)", i);
		}

		std::string decodedUri = tex.m_Uri;
		cgltf_decode_uri(decodedUri.data());
		decodedUri = decodedUri.c_str();

		std::filesystem::path fullPath = sceneDir / decodedUri;

		// DDS Prioritization: check for .dds version first
		std::filesystem::path ddsPath = fullPath;
		ddsPath.replace_extension(".dds");
		if (std::filesystem::exists(ddsPath))
		{
			fullPath = ddsPath;
		}

		if (!std::filesystem::exists(fullPath))
		{
			SDL_LOG_ASSERT_FAIL("Texture file not found", "[Scene] Texture file not found: %s", fullPath.string().c_str());
		}

		nvrhi::TextureDesc desc;
		std::unique_ptr<ITextureDataReader> imgData;
		if (!LoadTexture(fullPath.string(), desc, imgData))
		{
			SDL_LOG_ASSERT_FAIL("Texture load failed", "[Scene] Failed to load texture: %s", fullPath.string().c_str());
		}

		desc.isShaderResource = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = fullPath.string();
		
		tex.m_Handle = renderer->m_RHI->m_NvrhiDevice->createTexture(desc);

		nvrhi::CommandListHandle& cmd = threadCommandLists[threadIndex];
		const nvrhi::FormatInfo& info = nvrhi::getFormatInfo(desc.format);
		size_t offset = 0;
		for (uint32_t arraySlice = 0; arraySlice < desc.arraySize; ++arraySlice)
		{
			for (uint32_t mipLevel = 0; mipLevel < desc.mipLevels; ++mipLevel)
			{
				uint32_t mipWidth = std::max(1u, desc.width >> mipLevel);
				uint32_t mipHeight = std::max(1u, desc.height >> mipLevel);
				uint32_t mipDepth = std::max(1u, desc.depth >> mipLevel);

				uint32_t widthInBlocks = (mipWidth + info.blockSize - 1) / info.blockSize;
				uint32_t heightInBlocks = (mipHeight + info.blockSize - 1) / info.blockSize;

				size_t rowPitch = (size_t)widthInBlocks * info.bytesPerBlock;
				size_t slicePitch = (size_t)heightInBlocks * rowPitch;
				size_t subresourceSize = slicePitch * mipDepth;

				if (offset + subresourceSize > imgData->GetSize())
				{
					SDL_LOG_ASSERT_FAIL("Texture data overflow", "[Scene] Data overflow for texture %s at mip %u", fullPath.string().c_str(), mipLevel);
				}

				cmd->writeTexture(tex.m_Handle, arraySlice, mipLevel, static_cast<const uint8_t*>(imgData->GetData()) + offset, rowPitch, slicePitch);
				offset += subresourceSize;
			}
		}
	});

	for (size_t ti = 0; ti < scene.m_Textures.size(); ++ti)
	{
		SDL_assert(scene.m_Textures[ti].m_Handle);

		scene.m_Textures[ti].m_BindlessIndex = renderer->RegisterTexture(scene.m_Textures[ti].m_Handle);
		if (scene.m_Textures[ti].m_BindlessIndex == UINT32_MAX)
		{
			SDL_LOG_ASSERT_FAIL("Bindless texture registration failed", "[Scene] Bindless texture registration failed for %s", scene.m_Textures[ti].m_Uri.c_str());
		}
	}
}

static void UpdateMaterialsAndCreateConstants(Scene& scene, Renderer* renderer)
{
	SCOPED_TIMER("[Scene] MaterialConstants");

	for (Scene::Material& mat : scene.m_Materials)
	{
		if (mat.m_BaseColorTexture != -1)
			mat.m_AlbedoTextureIndex = scene.m_Textures[mat.m_BaseColorTexture].m_BindlessIndex;
		if (mat.m_NormalTexture != -1)
			mat.m_NormalTextureIndex = scene.m_Textures[mat.m_NormalTexture].m_BindlessIndex;
		if (mat.m_MetallicRoughnessTexture != -1)
			mat.m_RoughnessMetallicTextureIndex = scene.m_Textures[mat.m_MetallicRoughnessTexture].m_BindlessIndex;
		if (mat.m_EmissiveTexture != -1)
			mat.m_EmissiveTextureIndex = scene.m_Textures[mat.m_EmissiveTexture].m_BindlessIndex;
		
		// Map alpha mode
		// Already mapped during loading
	}

	std::vector<MaterialConstants> materialConstants;
	materialConstants.reserve(scene.m_Materials.size());
	for (const Scene::Material& mat : scene.m_Materials)
	{
		MaterialConstants mc{};
		mc.m_BaseColor = mat.m_BaseColorFactor;
		mc.m_EmissiveFactor = Vector4{ mat.m_EmissiveFactor.x, mat.m_EmissiveFactor.y, mat.m_EmissiveFactor.z, 1.0f };
		mc.m_RoughnessMetallic = Vector2{ mat.m_RoughnessFactor, mat.m_MetallicFactor };
		mc.m_TextureFlags = 0;
		if (mat.m_BaseColorTexture != -1) mc.m_TextureFlags |= TEXFLAG_ALBEDO;
		if (mat.m_NormalTexture != -1) mc.m_TextureFlags |= TEXFLAG_NORMAL;
		if (mat.m_MetallicRoughnessTexture != -1) mc.m_TextureFlags |= TEXFLAG_ROUGHNESS_METALLIC;
		if (mat.m_EmissiveTexture != -1) mc.m_TextureFlags |= TEXFLAG_EMISSIVE;
		mc.m_AlbedoTextureIndex = mat.m_AlbedoTextureIndex;
		mc.m_NormalTextureIndex = mat.m_NormalTextureIndex;
		mc.m_RoughnessMetallicTextureIndex = mat.m_RoughnessMetallicTextureIndex;
		mc.m_EmissiveTextureIndex = mat.m_EmissiveTextureIndex;
		mc.m_AlphaMode = mat.m_AlphaMode;
		mc.m_AlphaCutoff = mat.m_AlphaCutoff;
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

		if (mat.m_EmissiveTexture != -1)
			mc.m_EmissiveSamplerIndex = (uint32_t)scene.m_Textures[mat.m_EmissiveTexture].m_Sampler;
		else
			mc.m_EmissiveSamplerIndex = (uint32_t)Scene::Texture::Wrap;
		materialConstants.push_back(mc);
	}

	if (!materialConstants.empty())
	{
		nvrhi::BufferDesc matBufDesc = nvrhi::BufferDesc()
			.setByteSize(materialConstants.size() * sizeof(MaterialConstants))
			.setStructStride(sizeof(MaterialConstants))
			.setInitialState(nvrhi::ResourceStates::ShaderResource)
			.setKeepInitialState(true)
			.setDebugName("MaterialConstantsBuffer");
		scene.m_MaterialConstantsBuffer = renderer->m_RHI->m_NvrhiDevice->createBuffer(matBufDesc);

		ScopedCommandList cmd{ "Upload MaterialConstants" };
		cmd->writeBuffer(scene.m_MaterialConstantsBuffer, materialConstants.data(), materialConstants.size() * sizeof(MaterialConstants));
	}
}

static void ProcessCameras(const cgltf_data* data, Scene& scene)
{
	SCOPED_TIMER("[Scene] Cameras");
	for (cgltf_size i = 0; i < data->cameras_count; ++i)
	{
		const cgltf_camera& cgCam = data->cameras[i];
		Scene::Camera cam;
		cam.m_Name = cgCam.name ? cgCam.name : std::string();
		if (cgCam.type == cgltf_camera_type_perspective)
		{
			const cgltf_camera_perspective& p = cgCam.data.perspective;
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

static void ProcessLights(const cgltf_data* data, Scene& scene)
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

static void ProcessAnimations(const cgltf_data* data, Scene& scene)
{
	SCOPED_TIMER("[Scene] Animations");
	for (cgltf_size i = 0; i < data->animations_count; ++i)
	{
		const cgltf_animation& cgAnim = data->animations[i];
		Scene::Animation anim;
		anim.m_Name = cgAnim.name ? cgAnim.name : "Animation_" + std::to_string(i);

		for (cgltf_size si = 0; si < cgAnim.samplers_count; ++si)
		{
			const cgltf_animation_sampler& cgSampler = cgAnim.samplers[si];
			Scene::AnimationSampler sampler;

			switch (cgSampler.interpolation)
			{
			case cgltf_interpolation_type_linear: sampler.m_Interpolation = Scene::AnimationSampler::Interpolation::Linear; break;
			case cgltf_interpolation_type_step: sampler.m_Interpolation = Scene::AnimationSampler::Interpolation::Step; break;
			case cgltf_interpolation_type_cubic_spline: sampler.m_Interpolation = Scene::AnimationSampler::Interpolation::CubicSpline; break;
			default: sampler.m_Interpolation = Scene::AnimationSampler::Interpolation::Linear; break;
			}

			// Extract inputs (time)
			{
				size_t count = cgSampler.input->count;
				sampler.m_Inputs.resize(count);
				for (size_t k = 0; k < count; ++k)
					cgltf_accessor_read_float(cgSampler.input, k, &sampler.m_Inputs[k], 1);

				if (count > 0 && sampler.m_Inputs.back() > anim.m_Duration)
					anim.m_Duration = sampler.m_Inputs.back();
			}

			// Extract outputs (values)
			{
				size_t count = cgSampler.output->count;
				sampler.m_Outputs.resize(count);
				for (size_t k = 0; k < count; ++k)
				{
					float val[4] = { 0, 0, 0, 1 };
					cgltf_accessor_read_float(cgSampler.output, k, val, 4);
					sampler.m_Outputs[k] = Vector4{ val[0], val[1], val[2], val[3] };
				}
			}

			anim.m_Samplers.push_back(std::move(sampler));
		}

		for (cgltf_size ci = 0; ci < cgAnim.channels_count; ++ci)
		{
			const cgltf_animation_channel& cgChannel = cgAnim.channels[ci];
			if (!cgChannel.target_node) continue;
			if (cgChannel.target_path == cgltf_animation_path_type_weights) continue;

			Scene::AnimationChannel channel;
			channel.m_SamplerIndex = (int)cgltf_animation_sampler_index(&cgAnim, cgChannel.sampler);
			int nodeIdx = (int)cgltf_node_index(data, cgChannel.target_node);
			channel.m_NodeIndex = nodeIdx;
			scene.m_Nodes[nodeIdx].m_IsAnimated = true;

			switch (cgChannel.target_path)
			{
			case cgltf_animation_path_type_translation: channel.m_Path = Scene::AnimationChannel::Path::Translation; break;
			case cgltf_animation_path_type_rotation: channel.m_Path = Scene::AnimationChannel::Path::Rotation; break;
			case cgltf_animation_path_type_scale: channel.m_Path = Scene::AnimationChannel::Path::Scale; break;
			default: continue;
			}

			anim.m_Channels.push_back(std::move(channel));
		}

		scene.m_Animations.push_back(std::move(anim));
	}
}

static void ProcessMeshes(const cgltf_data* data, Scene& scene, std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices)
{
	SCOPED_TIMER("[Scene] Meshes");
	outVertices.clear();
	outIndices.clear();
	scene.m_MeshData.clear();
	scene.m_Meshlets.clear();
	scene.m_MeshletVertices.clear();
	scene.m_MeshletTriangles.clear();

	for (cgltf_size mi = 0; mi < data->meshes_count; ++mi)
	{
		const cgltf_mesh& cgMesh = data->meshes[mi];
		Scene::Mesh mesh;
		const size_t meshVertexStart = outVertices.size();

		for (cgltf_size pi = 0; pi < cgMesh.primitives_count; ++pi)
		{
			const cgltf_primitive& prim = cgMesh.primitives[pi];
			Scene::Primitive p;

			const cgltf_accessor* posAcc = nullptr;
			const cgltf_accessor* normAcc = nullptr;
			const cgltf_accessor* uvAcc = nullptr;

			for (cgltf_size ai = 0; ai < prim.attributes_count; ++ai)
			{
				const cgltf_attribute& attr = prim.attributes[ai];
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

			SDL_Log("[Scene] Processing Mesh %zu, Primitive %zu: %u vertices", mi, pi, p.m_VertexCount);

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

				outVertices.push_back(vx);
			}

			uint32_t baseIndexOffset = static_cast<uint32_t>(outIndices.size());
			uint32_t baseIndexCount = 0;
			if (prim.indices)
			{
				const cgltf_size idxCount = prim.indices->count;
				baseIndexCount = static_cast<uint32_t>(idxCount);
				for (cgltf_size k = 0; k < idxCount; ++k)
				{
					const cgltf_size rawIdx = cgltf_accessor_read_index(prim.indices, k);
					const uint32_t idx = static_cast<uint32_t>(rawIdx);
					outIndices.push_back(static_cast<uint32_t>(p.m_VertexOffset) + idx);
				}
			}

			// Generate meshlets and LODs
			if (baseIndexCount > 0)
			{
				const size_t max_vertices = kMaxMeshletVertices;
				const size_t max_triangles = kMaxMeshletTriangles;
				const float cone_weight = 0.25f;
				
				const uint32_t kIndexLimitForLODGeneration = 1024;
				const float kIndexReductionPercentageForLODGeneration = 0.5f;
				const size_t kMinimumIndicesForLODGeneration = 128;

				const float target_error_hq = 0.01f;     // Conservative HQ error
				const float kMaxErrorForLODGeneration = 0.10f; // Limit maximum allowed distortion
				const float kMinReductionRatio = 0.85f;  // Require at least 15% reduction to keep the LOD
				
				const float attribute_weights[3] = { 1.0f, 1.0f, 1.0f }; // Normal weights

				// We need indices relative to primitive's vertex start
				std::vector<uint32_t> localIndices(baseIndexCount);
				for (uint32_t i = 0; i < baseIndexCount; ++i)
				{
					localIndices[i] = outIndices[baseIndexOffset + i] - p.m_VertexOffset;
				}

				const float simplifyScale = meshopt_simplifyScale(&outVertices[p.m_VertexOffset].m_Pos.x, p.m_VertexCount, sizeof(Vertex));

				for (uint32_t lod = 0; lod < MAX_LOD_COUNT; ++lod)
				{
					std::vector<uint32_t> lodIndices;
					float lodError = 0.0f;

					if (lod == 0)
					{
						lodIndices = localIndices;
						lodError = 0.0f;
					}
					else
					{
						// Skip LOD generation for simple/low-poly meshes
						if (baseIndexCount < kIndexLimitForLODGeneration)
							break;

						size_t target_index_count = size_t(baseIndexCount * pow(kIndexReductionPercentageForLODGeneration, (float)lod));
						target_index_count = std::max(target_index_count, kMinimumIndicesForLODGeneration);

						if (target_index_count >= p.m_IndexCounts[lod - 1])
							break;

						lodIndices.resize(baseIndexCount);
						size_t new_index_count = meshopt_simplifyWithAttributes(
							lodIndices.data(),
							localIndices.data(), baseIndexCount,
							&outVertices[p.m_VertexOffset].m_Pos.x, p.m_VertexCount, sizeof(Vertex),
							&outVertices[p.m_VertexOffset].m_Normal.x, sizeof(Vertex),
							attribute_weights, 3,
							nullptr, target_index_count, target_error_hq, 0, &lodError);
						lodIndices.resize(new_index_count);

						// Stop if we reached the target index count
						if (new_index_count < kIndexLimitForLODGeneration)
							break;

						// Stop if simplification didn't reduce the mesh significantly or reached a high error
						if (new_index_count >= p.m_IndexCounts[lod - 1] * kMinReductionRatio || lodError > kMaxErrorForLODGeneration)
							break;
					}

					p.m_IndexOffsets[lod] = (uint32_t)outIndices.size();
					p.m_IndexCounts[lod] = (uint32_t)lodIndices.size();
					p.m_LODErrors[lod] = lodError * simplifyScale;

					for (uint32_t idx : lodIndices)
					{
						outIndices.push_back(idx + p.m_VertexOffset);
					}

					size_t max_meshlets = meshopt_buildMeshletsBound(lodIndices.size(), max_vertices, max_triangles);
					std::vector<meshopt_Meshlet> localMeshlets(max_meshlets);
					std::vector<unsigned int> meshlet_vertices(max_meshlets * max_vertices);
					std::vector<unsigned char> meshlet_triangles(max_meshlets * max_triangles * 3);

					const size_t meshlet_count = meshopt_buildMeshlets(localMeshlets.data(), meshlet_vertices.data(), meshlet_triangles.data(),
						lodIndices.data(), lodIndices.size(), &outVertices[p.m_VertexOffset].m_Pos.x, p.m_VertexCount, sizeof(Vertex),
						max_vertices, max_triangles, cone_weight);

					localMeshlets.resize(meshlet_count);

					p.m_MeshletOffsets[lod] = (uint32_t)scene.m_Meshlets.size();
					p.m_MeshletCounts[lod] = (uint32_t)meshlet_count;
					p.m_LODCount = lod + 1;

					SDL_Log("[Scene]   LOD %u: Indices %u, Meshlets %zu, Error %.6f (Abs: %.6f)",
						lod, p.m_IndexCounts[lod], meshlet_count, lodError, p.m_LODErrors[lod]);

					for (size_t i = 0; i < meshlet_count; ++i)
					{
						const meshopt_Meshlet& m = localMeshlets[i];

						// Optimization
						meshopt_optimizeMeshlet(&meshlet_vertices[m.vertex_offset], &meshlet_triangles[m.triangle_offset], m.triangle_count, m.vertex_count);

						// Bounds
						meshopt_Bounds bounds = meshopt_computeMeshletBounds(&meshlet_vertices[m.vertex_offset], &meshlet_triangles[m.triangle_offset],
							m.triangle_count, &outVertices[p.m_VertexOffset].m_Pos.x, p.m_VertexCount, sizeof(Vertex));

						Meshlet gpuMeshlet;
						gpuMeshlet.m_VertexOffset = (uint32_t)(scene.m_MeshletVertices.size());
						gpuMeshlet.m_TriangleOffset = (uint32_t)(scene.m_MeshletTriangles.size());
						gpuMeshlet.m_VertexCount = (uint32_t)m.vertex_count;
						gpuMeshlet.m_TriangleCount = (uint32_t)m.triangle_count;

						gpuMeshlet.m_Center = { bounds.center[0], bounds.center[1], bounds.center[2] };
						gpuMeshlet.m_Radius = bounds.radius;

						const uint32_t packedAxisX = (uint32_t)((bounds.cone_axis[0] + 1.0f) * 0.5f * UINT8_MAX);
						const uint32_t packedAxisY = (uint32_t)((bounds.cone_axis[1] + 1.0f) * 0.5f * UINT8_MAX);
						const uint32_t packedAxisZ = (uint32_t)((bounds.cone_axis[2] + 1.0f) * 0.5f * UINT8_MAX);
						const uint32_t packedCutoff = (uint32_t)(bounds.cone_cutoff_s8 * 2);

						gpuMeshlet.m_ConeAxisAndCutoff = packedAxisX | (packedAxisY << 8) | (packedAxisZ << 16) | (packedCutoff << 24);

						// Add vertices (adjust to global offset)
						for (uint32_t v = 0; v < m.vertex_count; ++v)
						{
							scene.m_MeshletVertices.push_back(meshlet_vertices[m.vertex_offset + v] + p.m_VertexOffset);
						}

						// Add triangles (packed: 3 indices per uint32_t)
						for (uint32_t t = 0; t < m.triangle_count; ++t)
						{
							uint32_t i0 = meshlet_triangles[m.triangle_offset + t * 3 + 0];
							uint32_t i1 = meshlet_triangles[m.triangle_offset + t * 3 + 1];
							uint32_t i2 = meshlet_triangles[m.triangle_offset + t * 3 + 2];
							scene.m_MeshletTriangles.push_back(i0 | (i1 << 8) | (i2 << 16));
						}

						scene.m_Meshlets.push_back(gpuMeshlet);
					}
				}
			}

			p.m_MaterialIndex = prim.material ? static_cast<int>(cgltf_material_index(data, prim.material)) : -1;

			// Extract MeshData
			p.m_MeshDataIndex = (uint32_t)scene.m_MeshData.size();
			mesh.m_Primitives.push_back(p);

			MeshData md{};
			md.m_LODCount = p.m_LODCount;
			for (uint32_t lod = 0; lod < p.m_LODCount; ++lod)
			{
				md.m_IndexOffsets[lod] = p.m_IndexOffsets[lod];
				md.m_IndexCounts[lod] = p.m_IndexCounts[lod];
				md.m_MeshletOffsets[lod] = p.m_MeshletOffsets[lod];
				md.m_MeshletCounts[lod] = p.m_MeshletCounts[lod];
				md.m_LODErrors[lod] = p.m_LODErrors[lod];
			}
			scene.m_MeshData.push_back(md);

			SDL_Log("  Primitive %zu: %u verts, %u indices -> %u LODs", pi, p.m_VertexCount, p.m_IndexCounts[0], p.m_LODCount);
		}

		Sphere s;
		if (outVertices.size() > meshVertexStart)
		{
			Sphere::CreateFromPoints(s, outVertices.size() - meshVertexStart, &outVertices[meshVertexStart].m_Pos, sizeof(Vertex));
		}
		else
		{
			s.Center = { 0,0,0 };
			s.Radius = 0;
		}
		mesh.m_Center = s.Center;
		mesh.m_Radius = s.Radius;

		const size_t primitives_count = cgMesh.primitives_count;
		SDL_Log("[Scene] Mesh %zu [%s]: %zu primitives", mi, cgMesh.name ? cgMesh.name : "unnamed", primitives_count);
		scene.m_Meshes.push_back(std::move(mesh));
	}

	SDL_Log("[Scene] ProcessMeshes completed:\n"
		"  Vertices:          %zu\n"
		"  Indices:           %zu\n"
		"  Meshlets:          %zu\n"
		"  Meshlet Vertices:  %zu\n"
		"  Meshlet Triangles: %zu",
		outVertices.size(), outIndices.size(), scene.m_Meshlets.size(), 
		scene.m_MeshletVertices.size(), scene.m_MeshletTriangles.size() / 3);
}

static void ProcessNodesAndHierarchy(const cgltf_data* data, Scene& scene)
{
	SCOPED_TIMER("[Scene] Nodes+Hierarchy");
	std::unordered_map<cgltf_size, int> nodeMap;
	for (cgltf_size ni = 0; ni < data->nodes_count; ++ni)
	{
		const cgltf_node& cn = data->nodes[ni];
		Scene::Node& node = scene.m_Nodes[ni];
		node.m_Name = cn.name ? cn.name : std::string();
		node.m_MeshIndex = cn.mesh ? static_cast<int>(cgltf_mesh_index(data, cn.mesh)) : -1;
		node.m_CameraIndex = cn.camera ? static_cast<int>(cgltf_camera_index(data, cn.camera)) : -1;
		node.m_LightIndex = cn.light ? static_cast<int>(cgltf_light_index(data, cn.light)) : -1;

		Matrix localOut{};
		if (cn.has_matrix)
		{
			for (int i = 0; i < 16; ++i)
				reinterpret_cast<float*>(&localOut)[i] = cn.matrix[i];

			// Decompose matrix to TRS in case it's animated later
			DirectX::XMVECTOR scale, rot, trans;
			DirectX::XMMatrixDecompose(&scale, &rot, &trans, DirectX::XMLoadFloat4x4(&localOut));
			DirectX::XMStoreFloat3(&node.m_Translation, trans);
			DirectX::XMStoreFloat4(&node.m_Rotation, rot);
			DirectX::XMStoreFloat3(&node.m_Scale, scale);
		}
		else
		{
			if (cn.has_translation)
				node.m_Translation = Vector3{ cn.translation[0], cn.translation[1], cn.translation[2] };
			if (cn.has_scale)
				node.m_Scale = Vector3{ cn.scale[0], cn.scale[1], cn.scale[2] };
			if (cn.has_rotation)
				node.m_Rotation = Quaternion{ cn.rotation[0], cn.rotation[1], cn.rotation[2], cn.rotation[3] };

			const DirectX::XMMATRIX localM = DirectX::XMMatrixScalingFromVector(DirectX::XMLoadFloat3(&node.m_Scale)) *
				DirectX::XMMatrixRotationQuaternion(DirectX::XMLoadFloat4(&node.m_Rotation)) *
				DirectX::XMMatrixTranslationFromVector(DirectX::XMLoadFloat3(&node.m_Translation));
			DirectX::XMStoreFloat4x4(&localOut, localM);
		}

		node.m_LocalTransform = localOut;
		node.m_WorldTransform = node.m_LocalTransform;

		cgltf_size nodeIndex = cgltf_node_index(data, &cn);
		nodeMap[nodeIndex] = static_cast<int>(ni);
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

	// Compute per-node bounding spheres by transforming mesh spheres into world space
	for (size_t ni = 0; ni < scene.m_Nodes.size(); ++ni)
	{
		scene.UpdateNodeBoundingSphere(static_cast<int>(ni));
	}
}

static void SetupDirectionalLightAndCamera(Scene& scene, Renderer* renderer)
{
	SCOPED_TIMER("[Scene] Setup Lights+Camera");
	for (const Scene::Light& light : scene.m_Lights)
	{
		if (light.m_Type == Scene::Light::Directional && light.m_NodeIndex >= 0 && light.m_NodeIndex < static_cast<int>(scene.m_Nodes.size()))
		{
			const Matrix& worldTransform = scene.m_Nodes[light.m_NodeIndex].m_WorldTransform;
			const DirectX::XMMATRIX m = DirectX::XMLoadFloat4x4(&worldTransform);
			const DirectX::XMVECTOR localDir = DirectX::XMVectorSet(0, 0, -1, 0);
			const DirectX::XMVECTOR worldDir = DirectX::XMVector3TransformNormal(localDir, m);
			DirectX::XMFLOAT3 dir;
			DirectX::XMStoreFloat3(&dir, DirectX::XMVector3Normalize(worldDir));
			const float yaw = atan2f(dir.x, dir.z);
			const float pitch = asinf(dir.y);
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
		desc.debugName = "Scene_VertexBuffer";
		scene.m_VertexBuffer = renderer->m_RHI->m_NvrhiDevice->createBuffer(desc);
	}

	if (ibytes > 0)
	{
		nvrhi::BufferDesc desc{};
		desc.byteSize = (uint32_t)ibytes;
		desc.isIndexBuffer = true;
		desc.initialState = nvrhi::ResourceStates::IndexBuffer;
		desc.keepInitialState = true;
		desc.debugName = "Scene_IndexBuffer";
		scene.m_IndexBuffer = renderer->m_RHI->m_NvrhiDevice->createBuffer(desc);
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
		desc.debugName = "Scene_MeshDataBuffer";
		scene.m_MeshDataBuffer = renderer->m_RHI->m_NvrhiDevice->createBuffer(desc);

		ScopedCommandList cmd{ "Upload Mesh Data" };
		cmd->writeBuffer(scene.m_MeshDataBuffer, scene.m_MeshData.data(), scene.m_MeshData.size() * sizeof(MeshData), 0);
	}

	// Create meshlet buffers
	if (!scene.m_Meshlets.empty())
	{
		nvrhi::BufferDesc desc{};
		desc.byteSize = (uint32_t)(scene.m_Meshlets.size() * sizeof(Meshlet));
		desc.structStride = sizeof(Meshlet);
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "Scene_MeshletBuffer";
		scene.m_MeshletBuffer = renderer->m_RHI->m_NvrhiDevice->createBuffer(desc);

		ScopedCommandList cmd{ "Upload Meshlets" };
		cmd->writeBuffer(scene.m_MeshletBuffer, scene.m_Meshlets.data(), scene.m_Meshlets.size() * sizeof(Meshlet), 0);
	}

	if (!scene.m_MeshletVertices.empty())
	{
		nvrhi::BufferDesc desc{};
		desc.byteSize = (uint32_t)(scene.m_MeshletVertices.size() * sizeof(uint32_t));
		desc.structStride = sizeof(uint32_t);
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "Scene_MeshletVerticesBuffer";
		scene.m_MeshletVerticesBuffer = renderer->m_RHI->m_NvrhiDevice->createBuffer(desc);

		ScopedCommandList cmd{ "Upload Meshlet Vertices" };
		cmd->writeBuffer(scene.m_MeshletVerticesBuffer, scene.m_MeshletVertices.data(), scene.m_MeshletVertices.size() * sizeof(uint32_t), 0);
	}

	if (!scene.m_MeshletTriangles.empty())
	{
		nvrhi::BufferDesc desc{};
		desc.byteSize = (uint32_t)(scene.m_MeshletTriangles.size() * sizeof(uint32_t));
		desc.structStride = sizeof(uint32_t);
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "Scene_MeshletTrianglesBuffer";
		scene.m_MeshletTrianglesBuffer = renderer->m_RHI->m_NvrhiDevice->createBuffer(desc);

		ScopedCommandList cmd{ "Upload Meshlet Triangles" };
		cmd->writeBuffer(scene.m_MeshletTrianglesBuffer, scene.m_MeshletTriangles.data(), scene.m_MeshletTriangles.size() * sizeof(uint32_t), 0);
	}

	// Create instance data buffer
	if (!scene.m_InstanceData.empty())
	{
		nvrhi::BufferDesc desc{};
		desc.byteSize = (uint32_t)(scene.m_InstanceData.size() * sizeof(PerInstanceData));
		desc.structStride = sizeof(PerInstanceData);
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "Scene_InstanceDataBuffer";
		scene.m_InstanceDataBuffer = renderer->m_RHI->m_NvrhiDevice->createBuffer(desc);
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

	const std::filesystem::path gltfPath(scenePath);
	const std::filesystem::path cachePath = gltfPath.parent_path() / (gltfPath.stem().string() + "_cooked.bin");
	const std::filesystem::path sceneDir = gltfPath.parent_path();

	Renderer* renderer = Renderer::GetInstance();
	std::vector<Vertex> allVertices;
	std::vector<uint32_t> allIndices;

	SCOPED_TIMER("[Scene] LoadScene Total");

	bool loadedFromCache = false;
	if (!Config::Get().m_SkipCache && std::filesystem::exists(cachePath))
	{
		const std::filesystem::file_time_type gltfTime = std::filesystem::last_write_time(gltfPath);
		const std::filesystem::file_time_type cacheTime = std::filesystem::last_write_time(cachePath);
		if (cacheTime > gltfTime)
		{
			SDL_Log("[Scene] Loading from binary cache: %s", cachePath.string().c_str());
			if (LoadFromCache(cachePath.string(), allVertices, allIndices))
			{
				loadedFromCache = true;
			}
			else
			{
				SDL_Log("[Scene] Cache load failed, falling back to glTF");
			}
		}
	}

	if (!loadedFromCache)
	{
		SDL_Log("[Scene] Loading glTF scene: %s", scenePath.c_str());

		const cgltf_options options{};
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

		m_Nodes.resize(data->nodes_count);
		ProcessMaterialsAndImages(data, *this);
		ProcessCameras(data, *this);
		ProcessLights(data, *this);
		ProcessAnimations(data, *this);
		ProcessMeshes(data, *this, allVertices, allIndices);
		ProcessNodesAndHierarchy(data, *this);

		// Identify dynamic nodes and sort them topologically
		std::function<void(int, bool)> IdentifyDynamic = [&](int idx, bool parentDynamic)
		{
			Scene::Node& node = m_Nodes[idx];
			node.m_IsDynamic = node.m_IsAnimated || parentDynamic;
			if (node.m_IsDynamic)
			{
				m_DynamicNodeIndices.push_back(idx);
			}
			for (int childIdx : node.m_Children)
			{
				IdentifyDynamic(childIdx, node.m_IsDynamic);
			}
		};

		for (int i = 0; i < (int)m_Nodes.size(); ++i)
		{
			if (m_Nodes[i].m_Parent == -1)
			{
				IdentifyDynamic(i, false);
			}
		}

		// Bucketize and fill instance data
		m_InstanceData.clear();
		struct InstInfo { PerInstanceData data; int nodeIdx; };
		std::vector<InstInfo> opaqueStatic, opaqueDynamic;
		std::vector<InstInfo> maskedStatic, maskedDynamic;
		std::vector<InstInfo> transparentStatic, transparentDynamic;

		for (int ni = 0; ni < (int)m_Nodes.size(); ++ni)
		{
			const Scene::Node& node = m_Nodes[ni];
			if (node.m_MeshIndex < 0) continue;
			const Scene::Mesh& mesh = m_Meshes[node.m_MeshIndex];
			for (const Scene::Primitive& prim : mesh.m_Primitives)
			{
				PerInstanceData inst{};
				inst.m_World = node.m_WorldTransform;
				inst.m_MaterialIndex = prim.m_MaterialIndex;
				inst.m_MeshDataIndex = prim.m_MeshDataIndex;
				inst.m_Center = node.m_Center;
				inst.m_Radius = node.m_Radius;

				uint32_t alphaMode = m_Materials[prim.m_MaterialIndex].m_AlphaMode;
				bool isDynamic = node.m_IsDynamic;

				if (alphaMode == ALPHA_MODE_OPAQUE) {
					if (isDynamic) opaqueDynamic.push_back({ inst, ni });
					else opaqueStatic.push_back({ inst, ni });
				}
				else if (alphaMode == ALPHA_MODE_MASK) {
					if (isDynamic) maskedDynamic.push_back({ inst, ni });
					else maskedStatic.push_back({ inst, ni });
				}
				else {
					if (isDynamic) transparentDynamic.push_back({ inst, ni });
					else transparentStatic.push_back({ inst, ni });
				}
			}
		}

		m_OpaqueBucket = { 0, (uint32_t)(opaqueStatic.size() + opaqueDynamic.size()) };
		m_MaskedBucket = { m_OpaqueBucket.m_BaseIndex + m_OpaqueBucket.m_Count, (uint32_t)(maskedStatic.size() + maskedDynamic.size()) };
		m_TransparentBucket = { m_MaskedBucket.m_BaseIndex + m_MaskedBucket.m_Count, (uint32_t)(transparentStatic.size() + transparentDynamic.size()) };

		auto PushInstances = [&](const std::vector<InstInfo>& infos)
		{
			for (const InstInfo& info : infos)
			{
				m_Nodes[info.nodeIdx].m_InstanceIndices.push_back((uint32_t)m_InstanceData.size());
				m_InstanceData.push_back(info.data);
			}
		};

		PushInstances(opaqueStatic);
		PushInstances(opaqueDynamic);
		PushInstances(maskedStatic);
		PushInstances(maskedDynamic);
		PushInstances(transparentStatic);
		PushInstances(transparentDynamic);

		SDL_Log("[Scene] Instances: Opaque: %u, Masked: %u, Transparent: %u", m_OpaqueBucket.m_Count, m_MaskedBucket.m_Count, m_TransparentBucket.m_Count);

		if (!Config::Get().m_SkipCache)
		{
			SDL_Log("[Scene] Saving binary cache: %s", cachePath.string().c_str());
			SaveToCache(cachePath.string(), allVertices, allIndices);
		}

		cgltf_free(data);
	}

	LoadTexturesFromImages(*this, sceneDir, renderer);
	UpdateMaterialsAndCreateConstants(*this, renderer);
	SetupDirectionalLightAndCamera(*this, renderer);
	CreateAndUploadGpuBuffers(*this, renderer, allVertices, allIndices);

	SDL_Log("[Scene] Loaded meshes: %zu, nodes: %zu", m_Meshes.size(), m_Nodes.size());

	return true;
}

void Scene::Update(float deltaTime)
{
	if (m_Animations.empty()) return;
	PROFILE_FUNCTION();

	for (Animation& anim : m_Animations)
	{
		anim.m_CurrentTime += deltaTime;
		if (anim.m_Duration > 0)
			anim.m_CurrentTime = fmodf(anim.m_CurrentTime, anim.m_Duration);
	}

	m_InstanceDirtyRange = { UINT32_MAX, 0 };

	for (const Animation& anim : m_Animations)
	{
		for (const Scene::AnimationChannel& channel : anim.m_Channels)
		{
			const AnimationSampler& sampler = anim.m_Samplers[channel.m_SamplerIndex];
			if (sampler.m_Inputs.empty()) continue;

			// GLTF spec: Animations are implicitly clamped to the range of their input values.
			// This ensures shorter channels don't loop until the entire animation duration is reached.
			float sampleTime = anim.m_CurrentTime;
			if (sampleTime < sampler.m_Inputs.front()) sampleTime = sampler.m_Inputs.front();
			if (sampleTime > sampler.m_Inputs.back()) sampleTime = sampler.m_Inputs.back();

			// Find keyframes
			uint32_t key0 = 0;
			for (uint32_t i = 0; i < (uint32_t)sampler.m_Inputs.size() - 1; ++i)
			{
				if (sampleTime >= sampler.m_Inputs[i])
					key0 = i;
			}
			uint32_t key1 = (key0 + 1 < (uint32_t)sampler.m_Inputs.size()) ? key0 + 1 : key0;

			float t = 0.0f;
			if (sampler.m_Interpolation == AnimationSampler::Interpolation::Step)
			{
				t = 0.0f;
			}
			else if (key0 != key1)
			{
				float dt = sampler.m_Inputs[key1] - sampler.m_Inputs[key0];
				t = (sampleTime - sampler.m_Inputs[key0]) / dt;
			}

			Node& node = m_Nodes[channel.m_NodeIndex];
			node.m_IsDirty = true; // Mark as dirty when TRS is changed by animation

			using namespace DirectX;
			if (channel.m_Path == AnimationChannel::Path::Translation)
			{
				Vector v0 = XMLoadFloat4(&sampler.m_Outputs[key0]);
				Vector v1 = XMLoadFloat4(&sampler.m_Outputs[key1]);
				XMStoreFloat3(&node.m_Translation, XMVectorLerp(v0, v1, t));
			}
			else if (channel.m_Path == AnimationChannel::Path::Rotation)
			{
				Vector q0 = XMLoadFloat4(&sampler.m_Outputs[key0]);
				Vector q1 = XMLoadFloat4(&sampler.m_Outputs[key1]);
				// For rotation, we should always use Slerp when not Step
				if (sampler.m_Interpolation == AnimationSampler::Interpolation::Step)
					XMStoreFloat4(&node.m_Rotation, q0);
				else
					XMStoreFloat4(&node.m_Rotation, XMQuaternionSlerp(q0, q1, t));
			}
			else if (channel.m_Path == AnimationChannel::Path::Scale)
			{
				Vector v0 = XMLoadFloat4(&sampler.m_Outputs[key0]);
				Vector v1 = XMLoadFloat4(&sampler.m_Outputs[key1]);
				XMStoreFloat3(&node.m_Scale, XMVectorLerp(v0, v1, t));
			}
		}
	}

	// Update only dynamic nodes in topological order
	for (int idx : m_DynamicNodeIndices)
	{
		Node& node = m_Nodes[idx];
		bool parentDirty = (node.m_Parent != -1 && m_Nodes[node.m_Parent].m_IsDirty);

		if (node.m_IsDirty || parentDirty)
		{
			using namespace DirectX;
			
			// Update local transform from TRS
			XMMATRIX localM = XMMatrixScalingFromVector(XMLoadFloat3(&node.m_Scale)) *
				XMMatrixRotationQuaternion(XMLoadFloat4(&node.m_Rotation)) *
				XMMatrixTranslationFromVector(XMLoadFloat3(&node.m_Translation));
			XMStoreFloat4x4(&node.m_LocalTransform, localM);

			// Update world transform
			XMMATRIX worldM;
			if (node.m_Parent != -1) {
				worldM = XMMatrixMultiply(localM, XMLoadFloat4x4(&m_Nodes[node.m_Parent].m_WorldTransform));
			} else {
				worldM = localM;
			}
			XMStoreFloat4x4(&node.m_WorldTransform, worldM);

			node.m_IsDirty = true; // Pass dirty state to children

			// Update bounding sphere
			UpdateNodeBoundingSphere(idx);

			// Sync instances
			for (uint32_t instIdx : node.m_InstanceIndices)
			{
				m_InstanceData[instIdx].m_World = node.m_WorldTransform;
				m_InstanceData[instIdx].m_Center = node.m_Center;
				m_InstanceData[instIdx].m_Radius = node.m_Radius;

				m_InstanceDirtyRange.first = std::min(m_InstanceDirtyRange.first, instIdx);
				m_InstanceDirtyRange.second = std::max(m_InstanceDirtyRange.second, instIdx);
			}
		}
		else
		{
			node.m_IsDirty = false;
		}
	}

	// Reset dirty flags for next frame (only for those that could have been set)
	for (int idx : m_DynamicNodeIndices)
	{
		m_Nodes[idx].m_IsDirty = false;
	}
}

void Scene::Shutdown()
{
	// Release GPU buffer handles so NVRHI can free underlying resources
	m_VertexBuffer = nullptr;
	m_IndexBuffer = nullptr;
	m_MaterialConstantsBuffer = nullptr;
	m_InstanceDataBuffer = nullptr;
	m_MeshDataBuffer = nullptr;
	m_MeshletBuffer = nullptr;
	m_MeshletVerticesBuffer = nullptr;
	m_MeshletTrianglesBuffer = nullptr;

	// Clear CPU-side containers
	m_Meshes.clear();
	m_Nodes.clear();
	m_Materials.clear();
	m_MeshData.clear();
	m_Meshlets.clear();
	m_MeshletVertices.clear();
	m_MeshletTriangles.clear();
	for (Scene::Texture& tex : m_Textures)
	{
		tex.m_Handle = nullptr;
	}
	m_Textures.clear();
	m_Cameras.clear();
	m_Lights.clear();
	m_InstanceData.clear();
}

void Scene::UpdateNodeBoundingSphere(int nodeIndex)
{
    Node& node = m_Nodes[nodeIndex];
    if (node.m_MeshIndex >= 0 && node.m_MeshIndex < (int)m_Meshes.size())
    {
        const Mesh& mesh = m_Meshes[node.m_MeshIndex];
        const DirectX::BoundingSphere localSphere(mesh.m_Center, mesh.m_Radius);
        DirectX::BoundingSphere worldSphere;
        localSphere.Transform(worldSphere, DirectX::XMLoadFloat4x4(&node.m_WorldTransform));
        node.m_Center = worldSphere.Center;
        node.m_Radius = worldSphere.Radius;
    }
}

static constexpr uint32_t kSceneCacheMagic = 0x59464C52; // "RLFY"
static constexpr uint32_t kSceneCacheVersion = 4;

bool Scene::SaveToCache(const std::string& cachePath, const std::vector<Vertex>& allVertices, const std::vector<uint32_t>& allIndices)
{
	std::ofstream os(cachePath, std::ios::binary);
	if (!os.is_open()) return false;

	WritePOD(os, kSceneCacheMagic);
	WritePOD(os, kSceneCacheVersion);

	// Buckets
	WritePOD(os, m_OpaqueBucket);
	WritePOD(os, m_MaskedBucket);
	WritePOD(os, m_TransparentBucket);

	// Meshes
	WritePOD(os, m_Meshes.size());
	for (const Scene::Mesh& mesh : m_Meshes)
	{
		WriteVector(os, mesh.m_Primitives);
		WritePOD(os, mesh.m_Center);
		WritePOD(os, mesh.m_Radius);
	}

	// Nodes
	WritePOD(os, m_Nodes.size());
	for (const Scene::Node& node : m_Nodes)
	{
		WriteString(os, node.m_Name);
		WritePOD(os, node.m_MeshIndex);
		WritePOD(os, node.m_Parent);
		WriteVector(os, node.m_Children);
		WritePOD(os, node.m_LocalTransform);
		WritePOD(os, node.m_WorldTransform);
		WritePOD(os, node.m_Center);
		WritePOD(os, node.m_Radius);
		WritePOD(os, node.m_CameraIndex);
		WritePOD(os, node.m_LightIndex);
		WritePOD(os, node.m_Translation);
		WritePOD(os, node.m_Rotation);
		WritePOD(os, node.m_Scale);
		WritePOD(os, node.m_IsAnimated);
		WritePOD(os, node.m_IsDynamic);
		WriteVector(os, node.m_InstanceIndices);
	}

	// Materials
	WritePOD(os, m_Materials.size());
	for (const Scene::Material& mat : m_Materials)
	{
		WriteString(os, mat.m_Name);
		WritePOD(os, mat.m_BaseColorFactor);
		WritePOD(os, mat.m_EmissiveFactor);
		WritePOD(os, mat.m_BaseColorTexture);
		WritePOD(os, mat.m_NormalTexture);
		WritePOD(os, mat.m_MetallicRoughnessTexture);
		WritePOD(os, mat.m_EmissiveTexture);
		WritePOD(os, mat.m_RoughnessFactor);
		WritePOD(os, mat.m_MetallicFactor);
		WritePOD(os, mat.m_AlbedoTextureIndex);
		WritePOD(os, mat.m_NormalTextureIndex);
		WritePOD(os, mat.m_RoughnessMetallicTextureIndex);
		WritePOD(os, mat.m_EmissiveTextureIndex);
		WritePOD(os, mat.m_AlphaMode);
		WritePOD(os, mat.m_AlphaCutoff);
	}

	// Textures
	WritePOD(os, m_Textures.size());
	for (const Scene::Texture& tex : m_Textures)
	{
		WriteString(os, tex.m_Uri);
		WritePOD(os, tex.m_Sampler);
	}

	// Cameras
	WritePOD(os, m_Cameras.size());
	for (const Scene::Camera& cam : m_Cameras)
	{
		WriteString(os, cam.m_Name);
		WritePOD(os, cam.m_Projection);
		WritePOD(os, cam.m_NodeIndex);
	}

	// Lights
	WritePOD(os, m_Lights.size());
	for (const Scene::Light& light : m_Lights)
	{
		WriteString(os, light.m_Name);
		WritePOD(os, light.m_Type);
		WritePOD(os, light.m_Color);
		WritePOD(os, light.m_Intensity);
		WritePOD(os, light.m_Range);
		WritePOD(os, light.m_SpotInnerConeAngle);
		WritePOD(os, light.m_SpotOuterConeAngle);
		WritePOD(os, light.m_NodeIndex);
	}

	WritePOD(os, m_DirectionalLight);

	WriteVector(os, m_InstanceData);
	WriteVector(os, m_MeshData);
	WriteVector(os, m_Meshlets);
	WriteVector(os, m_MeshletVertices);
	WriteVector(os, m_MeshletTriangles);
	WriteVector(os, allVertices);
	WriteVector(os, allIndices);

	// animations
	WriteVector(os, m_DynamicNodeIndices);
	WritePOD(os, m_Animations.size());
	for (const Animation& anim : m_Animations)
	{
		WriteString(os, anim.m_Name);
		WritePOD(os, anim.m_Duration);
		WritePOD(os, anim.m_CurrentTime);
		
		WritePOD(os, anim.m_Samplers.size());
		for (const AnimationSampler& sampler : anim.m_Samplers)
		{
			WritePOD(os, sampler.m_Interpolation);
			WriteVector(os, sampler.m_Inputs);
			WriteVector(os, sampler.m_Outputs);
		}
		WriteVector(os, anim.m_Channels);
	}

	return true;
}

bool Scene::LoadFromCache(const std::string& cachePath, std::vector<Vertex>& allVertices, std::vector<uint32_t>& allIndices)
{
	std::ifstream is(cachePath, std::ios::binary);
	if (!is.is_open()) return false;

	uint32_t magic;
	uint32_t version;
	ReadPOD(is, magic);
	ReadPOD(is, version);

	if (magic != kSceneCacheMagic || version != kSceneCacheVersion) 
	{
		SDL_Log("[Scene] Cache magic or version mismatch");
		return false;
	}

	// Buckets
	ReadPOD(is, m_OpaqueBucket);
	ReadPOD(is, m_MaskedBucket);
	ReadPOD(is, m_TransparentBucket);

	// Meshes
	size_t meshCount;
	ReadPOD(is, meshCount);
	m_Meshes.resize(meshCount);
	for (Scene::Mesh& mesh : m_Meshes)
	{
		ReadVector(is, mesh.m_Primitives);
		ReadPOD(is, mesh.m_Center);
		ReadPOD(is, mesh.m_Radius);
	}

	// Nodes
	size_t nodeCount;
	ReadPOD(is, nodeCount);
	m_Nodes.resize(nodeCount);
	for (Scene::Node& node : m_Nodes)
	{
		ReadString(is, node.m_Name);
		ReadPOD(is, node.m_MeshIndex);
		ReadPOD(is, node.m_Parent);
		ReadVector(is, node.m_Children);
		ReadPOD(is, node.m_LocalTransform);
		ReadPOD(is, node.m_WorldTransform);
		ReadPOD(is, node.m_Center);
		ReadPOD(is, node.m_Radius);
		ReadPOD(is, node.m_CameraIndex);
		ReadPOD(is, node.m_LightIndex);
		ReadPOD(is, node.m_Translation);
		ReadPOD(is, node.m_Rotation);
		ReadPOD(is, node.m_Scale);
		ReadPOD(is, node.m_IsAnimated);
		ReadPOD(is, node.m_IsDynamic);
		ReadVector(is, node.m_InstanceIndices);
	}

	// Materials
	size_t matCount;
	ReadPOD(is, matCount);
	m_Materials.resize(matCount);
	for (Scene::Material& mat : m_Materials)
	{
		ReadString(is, mat.m_Name);
		ReadPOD(is, mat.m_BaseColorFactor);
		ReadPOD(is, mat.m_EmissiveFactor);
		ReadPOD(is, mat.m_BaseColorTexture);
		ReadPOD(is, mat.m_NormalTexture);
		ReadPOD(is, mat.m_MetallicRoughnessTexture);
		ReadPOD(is, mat.m_EmissiveTexture);
		ReadPOD(is, mat.m_RoughnessFactor);
		ReadPOD(is, mat.m_MetallicFactor);
		ReadPOD(is, mat.m_AlbedoTextureIndex);
		ReadPOD(is, mat.m_NormalTextureIndex);
		ReadPOD(is, mat.m_RoughnessMetallicTextureIndex);
		ReadPOD(is, mat.m_EmissiveTextureIndex);
		ReadPOD(is, mat.m_AlphaMode);
		ReadPOD(is, mat.m_AlphaCutoff);
	}

	// Textures
	size_t texCount;
	ReadPOD(is, texCount);
	m_Textures.resize(texCount);
	for (Scene::Texture& tex : m_Textures)
	{
		ReadString(is, tex.m_Uri);
		ReadPOD(is, tex.m_Sampler);
	}

	// Cameras
	size_t camCount;
	ReadPOD(is, camCount);
	m_Cameras.resize(camCount);
	for (Scene::Camera& cam : m_Cameras)
	{
		ReadString(is, cam.m_Name);
		ReadPOD(is, cam.m_Projection);
		ReadPOD(is, cam.m_NodeIndex);
	}

	// Lights
	size_t lightCount;
	ReadPOD(is, lightCount);
	m_Lights.resize(lightCount);
	for (Scene::Light& light : m_Lights)
	{
		ReadString(is, light.m_Name);
		ReadPOD(is, light.m_Type);
		ReadPOD(is, light.m_Color);
		ReadPOD(is, light.m_Intensity);
		ReadPOD(is, light.m_Range);
		ReadPOD(is, light.m_SpotInnerConeAngle);
		ReadPOD(is, light.m_SpotOuterConeAngle);
		ReadPOD(is, light.m_NodeIndex);
	}

	ReadPOD(is, m_DirectionalLight);

	ReadVector(is, m_InstanceData);
	ReadVector(is, m_MeshData);
	ReadVector(is, m_Meshlets);
	ReadVector(is, m_MeshletVertices);
	ReadVector(is, m_MeshletTriangles);
	ReadVector(is, allVertices);
	ReadVector(is, allIndices);

	// animations
	ReadVector(is, m_DynamicNodeIndices);
	size_t animCount;
	ReadPOD(is, animCount);
	m_Animations.resize(animCount);
	for (Animation& anim : m_Animations)
	{
		ReadString(is, anim.m_Name);
		ReadPOD(is, anim.m_Duration);
		ReadPOD(is, anim.m_CurrentTime);

		size_t samplerCount;
		ReadPOD(is, samplerCount);
		anim.m_Samplers.resize(samplerCount);
		for (AnimationSampler& sampler : anim.m_Samplers)
		{
			ReadPOD(is, sampler.m_Interpolation);
			ReadVector(is, sampler.m_Inputs);
			ReadVector(is, sampler.m_Outputs);
		}
		ReadVector(is, anim.m_Channels);
	}

	return true;
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
