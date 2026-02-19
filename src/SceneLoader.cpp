#include "SceneLoader.h"
#include "Config.h"
#include "Renderer.h"
#include "CommonResources.h"
#include "Utilities.h"
#include "TextureLoader.h"
#include "shaders/ShaderShared.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include "meshoptimizer.h"

// --- JSON Parsing Helpers (jsmn wrapper) ---
struct JsonContext
{
	const char* json;
	jsmntok_t* tokens;
	int numTokens;
};

static std::string json_get_string(const JsonContext& ctx, int tokenIdx)
{
	if (tokenIdx < 0 || tokenIdx >= ctx.numTokens || ctx.tokens[tokenIdx].type != JSMN_STRING)
	{
		SDL_LOG_ASSERT_FAIL("Invalid JSON token for string", "[SceneLoader] Invalid JSON token for string at index %d", tokenIdx);
	}
	return std::string(ctx.json + ctx.tokens[tokenIdx].start, ctx.tokens[tokenIdx].end - ctx.tokens[tokenIdx].start);
}

static float json_get_float(const JsonContext& ctx, int tokenIdx)
{
	if (tokenIdx < 0 || tokenIdx >= ctx.numTokens || ctx.tokens[tokenIdx].type != JSMN_PRIMITIVE)
	{
		SDL_LOG_ASSERT_FAIL("Invalid JSON token for float", "[SceneLoader] Invalid JSON token for float at index %d", tokenIdx);
	}
	try
	{
		return std::stof(std::string(ctx.json + ctx.tokens[tokenIdx].start, ctx.tokens[tokenIdx].end - ctx.tokens[tokenIdx].start));
	}
	catch (...)
	{
		return 0.0f;
	}
}

static bool json_get_bool(const JsonContext& ctx, int tokenIdx)
{
	if (tokenIdx < 0 || tokenIdx >= ctx.numTokens || ctx.tokens[tokenIdx].type != JSMN_PRIMITIVE)
	{
		SDL_LOG_ASSERT_FAIL("Invalid JSON token for bool", "[SceneLoader] Invalid JSON token for bool at index %d", tokenIdx);
	}
	std::string s(ctx.json + ctx.tokens[tokenIdx].start, ctx.tokens[tokenIdx].end - ctx.tokens[tokenIdx].start);
	return s == "true";
}

static Vector3 json_get_vec3(const JsonContext& ctx, int tokenIdx)
{
	if (tokenIdx < 0 || tokenIdx >= ctx.numTokens || ctx.tokens[tokenIdx].type != JSMN_ARRAY || ctx.tokens[tokenIdx].size != 3) 
	{
		SDL_LOG_ASSERT_FAIL("Invalid JSON token for vec3", "[SceneLoader] Invalid JSON token for vec3 at index %d", tokenIdx);
	}
	return Vector3(json_get_float(ctx, tokenIdx + 1), json_get_float(ctx, tokenIdx + 2), json_get_float(ctx, tokenIdx + 3));
}

static Quaternion json_get_quat(const JsonContext& ctx, int tokenIdx)
{
	if (tokenIdx < 0 || tokenIdx >= ctx.numTokens || ctx.tokens[tokenIdx].type != JSMN_ARRAY)
	{
		SDL_LOG_ASSERT_FAIL("Invalid JSON token for quat", "[SceneLoader] Invalid JSON token for quat at index %d", tokenIdx);
	}

	// possible to get a single '0' for Rotation type to specify no rotation.
	if (ctx.tokens[tokenIdx].size == 1)
	{
		const float angle = json_get_float(ctx, tokenIdx + 1);
		SDL_assert(angle == 0.0f);

		return Quaternion{ 0.0f, 0.0f, 0.0f, 1.0f };
	}

	return Quaternion(json_get_float(ctx, tokenIdx + 1), json_get_float(ctx, tokenIdx + 2), json_get_float(ctx, tokenIdx + 3), json_get_float(ctx, tokenIdx + 4));
}

static bool json_strcmp(const JsonContext& ctx, int tokenIdx, const char* str)
{
	if (tokenIdx < 0 || tokenIdx >= ctx.numTokens || ctx.tokens[tokenIdx].type != JSMN_STRING)
	{
		SDL_LOG_ASSERT_FAIL("Invalid JSON token for string comparison", "[SceneLoader] Invalid JSON token for string comparison at index %d", tokenIdx);
	}
	int len = ctx.tokens[tokenIdx].end - ctx.tokens[tokenIdx].start;
	return (int)strlen(str) == len && strncmp(ctx.json + ctx.tokens[tokenIdx].start, str, len) == 0;
}

static nvrhi::TextureHandle LoadAndRegisterEnvMap(const std::string& path, uint32_t index, const char* name)
{
	nvrhi::TextureDesc desc;
	std::unique_ptr<ITextureDataReader> imgData;
	if (!LoadTexture(path, desc, imgData))
	{
		SDL_LOG_ASSERT_FAIL("Failed to load environment map", "[SceneLoader] Failed to load environment map: %s", path.c_str());
	}

	desc.debugName = path;
	nvrhi::TextureHandle tex = Renderer::GetInstance()->m_RHI->m_NvrhiDevice->createTexture(desc);

	nvrhi::CommandListHandle cmd = Renderer::GetInstance()->AcquireCommandList();
	ScopedCommandList scopedCmd{ cmd, name };

	UploadTexture(scopedCmd, tex, desc, imgData->GetData(), imgData->GetSize());

	Renderer::GetInstance()->RegisterTextureAtIndex(index, tex);

	return tex;
}

bool SceneLoader::LoadJSONScene(Scene& scene, const std::string& scenePath, std::vector<VertexQuantized>& allVerticesQuantized, std::vector<uint32_t>& allIndices)
{
	SCOPED_TIMER("[Scene] LoadJSONScene");

	SDL_Log("[Scene] Starting to load JSON scene: %s", scenePath.c_str());

	std::ifstream file(scenePath, std::ios::ate | std::ios::binary);
	if (!file.is_open())
	{
		SDL_LOG_ASSERT_FAIL("Failed to open JSON scene file", "[Scene] Failed to open JSON scene file: %s", scenePath.c_str());
		return false;
	}

	size_t fileSize = (size_t)file.tellg();
	std::string jsonContent(fileSize, '\0');
	file.seekg(0);
	file.read(jsonContent.data(), fileSize);
	file.close();

	SDL_Log("[Scene] JSON file loaded, size: %zu bytes", fileSize);

	jsmn_parser parser;
	jsmn_init(&parser);
	int tokenCount = jsmn_parse(&parser, jsonContent.c_str(), jsonContent.size(), nullptr, 0);
	if (tokenCount < 0)
	{
		SDL_LOG_ASSERT_FAIL("Failed to parse JSON scene file", "[Scene] Failed to parse JSON scene file: %s (error: %d)", scenePath.c_str(), tokenCount);
		return false;
	}

	std::vector<jsmntok_t> tokens(tokenCount);
	jsmn_init(&parser);
	jsmn_parse(&parser, jsonContent.c_str(), jsonContent.size(), tokens.data(), tokenCount);

	SDL_Log("[Scene] JSON parsed, %d tokens", tokenCount);

	JsonContext ctx{ jsonContent.c_str(), tokens.data(), tokenCount };
	if (tokens[0].type != JSMN_OBJECT)
	{
		SDL_LOG_ASSERT_FAIL("Invalid JSON scene format", "[Scene] Invalid JSON scene format: root should be an object in file %s", scenePath.c_str());
		return false;
	}

	int modelsTokenIdx = -1;
	int graphTokenIdx = -1;

	for (int i = 1; i < tokenCount; )
	{
		if (json_strcmp(ctx, i, "models"))
		{
			modelsTokenIdx = i + 1;
			i = cgltf_skip_json(tokens.data(), i + 1);
		}
		else if (json_strcmp(ctx, i, "graph"))
		{
			graphTokenIdx = i + 1;
			i = cgltf_skip_json(tokens.data(), i + 1);
		}
		else
		{
			i = cgltf_skip_json(tokens.data(), i + 1);
		}
	}

	std::filesystem::path sceneDir = std::filesystem::path(scenePath).parent_path();

	// 1. Load models
	struct ModelInfo
	{
		int nodeOffset;
		int meshOffset;
		int cameraOffset;
		int lightOffset;
		int materialOffset;
		int textureOffset;
	};
	std::vector<ModelInfo> loadedModels;

	if (modelsTokenIdx != -1 && tokens[modelsTokenIdx].type == JSMN_ARRAY)
	{
		int numModels = tokens[modelsTokenIdx].size;
		SDL_Log("[Scene] Loading %d models", numModels);
		int currentToken = modelsTokenIdx + 1;
		for (int m = 0; m < numModels; ++m)
		{
			std::string modelRelPath = json_get_string(ctx, currentToken);
			std::filesystem::path modelFullPath = sceneDir / modelRelPath;

			ModelInfo info;
			info.nodeOffset = (int)scene.m_Nodes.size();
			info.meshOffset = (int)scene.m_Meshes.size();
			info.cameraOffset = (int)scene.m_Cameras.size();
			info.lightOffset = (int)scene.m_Lights.size();
			info.materialOffset = (int)scene.m_Materials.size();
			info.textureOffset = (int)scene.m_Textures.size();

			SDL_Log("[Scene] Loading model: %s", modelRelPath.c_str());
			if (!LoadGLTFScene(scene, modelFullPath.string(), allVerticesQuantized, allIndices))
			{
				SDL_LOG_ASSERT_FAIL("Failed to load model", "[Scene] Failed to load model: %s", modelFullPath.string().c_str());
			}
			else
			{
				// Adjust URIs of newly added textures to be relative to the JSON scene root
				std::filesystem::path modelDir = modelFullPath.parent_path();
				std::filesystem::path relativeModelDir = std::filesystem::relative(modelDir, sceneDir);

				for (size_t i = info.textureOffset; i < scene.m_Textures.size(); ++i)
				{
					if (!scene.m_Textures[i].m_Uri.empty())
					{
						scene.m_Textures[i].m_Uri = (relativeModelDir / scene.m_Textures[i].m_Uri).generic_string();
					}
				}

				loadedModels.push_back(info);
			}

			currentToken = cgltf_skip_json(tokens.data(), currentToken);
		}
	}

	int totalModelNodes = (int)scene.m_Nodes.size();

	// 2. Parse graph and reconstruct unified scene hierarchy
	if (graphTokenIdx != -1 && tokens[graphTokenIdx].type == JSMN_ARRAY)
	{
		// Process graph nodes recursively
		auto ParseGraphNode = [&](auto self, int tokenIdx, int parentIdx) -> void
		{
			if (tokens[tokenIdx].type != JSMN_OBJECT) return;

			int nodeIdx = (int)scene.m_Nodes.size();
			scene.m_Nodes.emplace_back();
			Scene::Node& newNode = scene.m_Nodes.back();
			newNode.m_Parent = parentIdx;
			if (parentIdx != -1)
			{
				scene.m_Nodes[parentIdx].m_Children.push_back(nodeIdx);
			}

			int modelIdx = -1;
			int childrenIdx = -1;

			int numKeys = tokens[tokenIdx].size;
			int t = tokenIdx + 1;
			for (int k = 0; k < numKeys; ++k)
			{
				if (json_strcmp(ctx, t, "name"))
				{
					newNode.m_Name = json_get_string(ctx, t + 1);
				}
				else if (json_strcmp(ctx, t, "translation"))
				{
					newNode.m_Translation = json_get_vec3(ctx, t + 1);
				}
				else if (json_strcmp(ctx, t, "rotation"))
				{
					newNode.m_Rotation = json_get_quat(ctx, t + 1);
				}
				else if (json_strcmp(ctx, t, "scale"))
				{
					newNode.m_Scale = json_get_vec3(ctx, t + 1);
				}
				else if (json_strcmp(ctx, t, "model"))
				{
					modelIdx = (int)json_get_float(ctx, t + 1);
				}
				else if (json_strcmp(ctx, t, "children"))
				{
					childrenIdx = t + 1;
				}
				else if (json_strcmp(ctx, t, "type"))
				{
					std::string type = json_get_string(ctx, t + 1);
					int objToken = tokenIdx;
					int nKeys = tokens[objToken].size;
					int ctStart = objToken + 1;

					if (type == "EnvironmentLight")
					{
						int ct = ctStart;
						std::string envMapRelPath;
						for (int ki = 0; ki < nKeys; ++ki)
						{
							if (json_strcmp(ctx, ct, "path")) envMapRelPath = json_get_string(ctx, ct + 1);
							ct = cgltf_skip_json(tokens.data(), ct + 1);
						}

						if (!envMapRelPath.empty())
						{
							std::filesystem::path envMapPath = sceneDir / envMapRelPath;
							std::string stem = envMapPath.stem().string();
							std::filesystem::path parent = envMapPath.parent_path();

							scene.m_IrradianceTexturePath = (parent / (stem + "_irradiance.dds")).string();
							SDL_assert(std::filesystem::exists(scene.m_IrradianceTexturePath));

							scene.m_RadianceTexturePath = (parent / (stem + "_radiance.dds")).string();
							SDL_assert(std::filesystem::exists(scene.m_RadianceTexturePath));
						}
					}
					else if (type == "PerspectiveCamera" || type == "PerspectiveCameraEx")
					{
						scene.m_Cameras.emplace_back();
						Scene::Camera& cam = scene.m_Cameras.back();
						cam.m_Name = newNode.m_Name;
						cam.m_NodeIndex = nodeIdx;
						cam.m_Projection.nearZ = 0.1f;
						newNode.m_CameraIndex = (int)scene.m_Cameras.size() - 1;

						int ct = ctStart;
						for (int ki = 0; ki < nKeys; ++ki)
						{
							if (json_strcmp(ctx, ct, "verticalFov")) cam.m_Projection.fovY = json_get_float(ctx, ct + 1);
							else if (json_strcmp(ctx, ct, "zNear")) cam.m_Projection.nearZ = json_get_float(ctx, ct + 1);
							else if (json_strcmp(ctx, ct, "exposureValue")) cam.m_ExposureValue = json_get_float(ctx, ct + 1);
							else if (json_strcmp(ctx, ct, "exposureCompensation")) cam.m_ExposureCompensation = json_get_float(ctx, ct + 1);
							else if (json_strcmp(ctx, ct, "exposureValueMin")) cam.m_ExposureValueMin = json_get_float(ctx, ct + 1);
							else if (json_strcmp(ctx, ct, "exposureValueMax")) cam.m_ExposureValueMax = json_get_float(ctx, ct + 1);
							ct = cgltf_skip_json(tokens.data(), ct + 1);
						}
					}
					else if (type == "DirectionalLight")
					{
						scene.m_Lights.emplace_back();
						Scene::Light& light = scene.m_Lights.back();
						light.m_Type = Scene::Light::Directional;
						light.m_Name = newNode.m_Name;
						light.m_NodeIndex = nodeIdx;
						newNode.m_LightIndex = (int)scene.m_Lights.size() - 1;

						int ct = ctStart;
						for (int ki = 0; ki < nKeys; ++ki)
						{
							if (json_strcmp(ctx, ct, "irradiance")) light.m_Intensity = json_get_float(ctx, ct + 1);
							else if (json_strcmp(ctx, ct, "angularSize")) light.m_AngularSize = json_get_float(ctx, ct + 1);
							else if (json_strcmp(ctx, ct, "color")) light.m_Color = json_get_vec3(ctx, ct + 1);
							ct = cgltf_skip_json(tokens.data(), ct + 1);
						}
					}
					else if (type == "SpotLight")
					{
						scene.m_Lights.emplace_back();
						Scene::Light& light = scene.m_Lights.back();
						light.m_Type = Scene::Light::Spot;
						light.m_Name = newNode.m_Name;
						light.m_NodeIndex = nodeIdx;
						newNode.m_LightIndex = (int)scene.m_Lights.size() - 1;

						int ct = ctStart;
						for (int ki = 0; ki < nKeys; ++ki)
						{
							if (json_strcmp(ctx, ct, "intensity")) light.m_Intensity = json_get_float(ctx, ct + 1);
							else if (json_strcmp(ctx, ct, "innerAngle")) light.m_SpotInnerConeAngle = json_get_float(ctx, ct + 1);
							else if (json_strcmp(ctx, ct, "outerAngle")) light.m_SpotOuterConeAngle = json_get_float(ctx, ct + 1);
							else if (json_strcmp(ctx, ct, "radius")) light.m_Radius = json_get_float(ctx, ct + 1);
							else if (json_strcmp(ctx, ct, "range")) light.m_Range = json_get_float(ctx, ct + 1);
							else if (json_strcmp(ctx, ct, "color")) light.m_Color = json_get_vec3(ctx, ct + 1);
							ct = cgltf_skip_json(tokens.data(), ct + 1);
						}
					}
				}

				t = cgltf_skip_json(tokens.data(), t + 1);
			}

			// Update transform
			const DirectX::XMMATRIX localM = DirectX::XMMatrixScalingFromVector(DirectX::XMLoadFloat3(&newNode.m_Scale)) *
				DirectX::XMMatrixRotationQuaternion(DirectX::XMLoadFloat4(&newNode.m_Rotation)) *
				DirectX::XMMatrixTranslationFromVector(DirectX::XMLoadFloat3(&newNode.m_Translation));
			DirectX::XMStoreFloat4x4(&newNode.m_LocalTransform, localM);
			newNode.m_WorldTransform = newNode.m_LocalTransform;

			// Parent loaded model roots to this node
			if (modelIdx >= 0 && modelIdx < (int)loadedModels.size())
			{
				const ModelInfo& info = loadedModels[modelIdx];
				// Roots of a GLTF are nodes with parent -1 in their own range
				int nextModelNodeOffset = (modelIdx + 1 < (int)loadedModels.size()) ? loadedModels[modelIdx + 1].nodeOffset : totalModelNodes;
				
				for (int i = info.nodeOffset; i < nextModelNodeOffset; ++i)
				{
					if (scene.m_Nodes[i].m_Parent == -1)
					{
						scene.m_Nodes[i].m_Parent = nodeIdx;
						scene.m_Nodes[nodeIdx].m_Children.push_back(i);
					}
				}
			}

			if (childrenIdx != -1 && tokens[childrenIdx].type == JSMN_ARRAY)
			{
				int numChildren = tokens[childrenIdx].size;
				int ct = childrenIdx + 1;
				for (int c = 0; c < numChildren; ++c)
				{
					self(self, ct, nodeIdx);
					ct = cgltf_skip_json(tokens.data(), ct);
				}
			}
		};

		int numGraphRoots = tokens[graphTokenIdx].size;
		SDL_Log("[Scene] Processing graph with %d roots", numGraphRoots);
		int currentToken = graphTokenIdx + 1;
		for (int r = 0; r < numGraphRoots; ++r)
		{
			ParseGraphNode(ParseGraphNode, currentToken, -1);
			currentToken = cgltf_skip_json(tokens.data(), currentToken);
		}
	}

	// Compute world transforms for all roots
	for (size_t i = 0; i < scene.m_Nodes.size(); ++i)
	{
		if (scene.m_Nodes[i].m_Parent == -1)
		{
			Matrix identity{};
			DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
			ComputeWorldTransforms(scene, (int)i, identity);
		}
	}

	// Update bounding spheres
	for (size_t ni = 0; ni < scene.m_Nodes.size(); ++ni)
	{
		scene.UpdateNodeBoundingSphere((int)ni);
	}

	SDL_Log("[Scene] JSON scene loaded successfully");
	return true;
}

void SceneLoader::ApplyEnvironmentLights(Scene& scene)
{
	if (scene.m_IrradianceTexturePath.empty() || scene.m_RadianceTexturePath.empty())
	{
		return;
	}

	SDL_assert(std::filesystem::exists(scene.m_IrradianceTexturePath));
	SDL_assert(std::filesystem::exists(scene.m_RadianceTexturePath));

	scene.m_IrradianceTexture = ::LoadAndRegisterEnvMap(scene.m_IrradianceTexturePath, DEFAULT_TEXTURE_IRRADIANCE, "Upload Env Irradiance");
	scene.m_RadianceTexture = ::LoadAndRegisterEnvMap(scene.m_RadianceTexturePath, DEFAULT_TEXTURE_RADIANCE, "Upload Env Radiance");
}

const char* SceneLoader::cgltf_result_tostring(cgltf_result result)
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
cgltf_result SceneLoader::decompressMeshopt(cgltf_data* data)
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

// Recursively compute world transforms for a Scene instance
void SceneLoader::ComputeWorldTransforms(Scene& scene, int nodeIndex, const Matrix& parent)
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
void SceneLoader::SetTextureAndSampler(const cgltf_texture* tex, int& textureIndex, const cgltf_data* data, int textureOffset)
{
    if (tex && !Config::Get().m_SkipTextures)
    {
        textureIndex = static_cast<int>(cgltf_texture_index(data, tex)) + textureOffset;
    }
}

void SceneLoader::ProcessMaterialsAndImages(const cgltf_data* data, Scene& scene, const std::filesystem::path& sceneDir, const SceneOffsets& offsets)
{
	SCOPED_TIMER("[Scene] Materials+Images");

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

			SetTextureAndSampler(sg.diffuse_texture.texture, scene.m_Materials.back().m_BaseColorTexture, data, offsets.textureOffset);
			SetTextureAndSampler(sg.specular_glossiness_texture.texture, scene.m_Materials.back().m_MetallicRoughnessTexture, data, offsets.textureOffset);
		}
		else if (data->materials[i].has_pbr_metallic_roughness)
		{
			scene.m_Materials.back().m_BaseColorFactor.x = pbr.base_color_factor[0];
			scene.m_Materials.back().m_BaseColorFactor.y = pbr.base_color_factor[1];
			scene.m_Materials.back().m_BaseColorFactor.z = pbr.base_color_factor[2];
			scene.m_Materials.back().m_BaseColorFactor.w = pbr.base_color_factor[3];
			SetTextureAndSampler(pbr.base_color_texture.texture, scene.m_Materials.back().m_BaseColorTexture, data, offsets.textureOffset);

			float metallic = pbr.metallic_factor;
			if (pbr.metallic_roughness_texture.texture == NULL && metallic == 1.0f)
				metallic = 0.0f;
			scene.m_Materials.back().m_RoughnessFactor = pbr.roughness_factor;
			scene.m_Materials.back().m_MetallicFactor = metallic;

			SetTextureAndSampler(pbr.metallic_roughness_texture.texture, scene.m_Materials.back().m_MetallicRoughnessTexture, data, offsets.textureOffset);
		}
		else
		{
			// Default values
			scene.m_Materials.back().m_BaseColorFactor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
			scene.m_Materials.back().m_MetallicFactor = 0.0f;
			scene.m_Materials.back().m_RoughnessFactor = 1.0f;
		}

		SetTextureAndSampler(data->materials[i].normal_texture.texture, scene.m_Materials.back().m_NormalTexture, data, offsets.textureOffset);
		SetTextureAndSampler(data->materials[i].emissive_texture.texture, scene.m_Materials.back().m_EmissiveTexture, data, offsets.textureOffset);
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
			scene.m_Materials.back().m_TransmissionFactor = data->materials[i].transmission.transmission_factor;
		}

		if (data->materials[i].has_ior)
		{
			scene.m_Materials.back().m_IOR = data->materials[i].ior.ior;
		}

		if (data->materials[i].has_volume)
		{
			scene.m_Materials.back().m_ThicknessFactor = data->materials[i].volume.thickness_factor;
			scene.m_Materials.back().m_AttenuationDistance = data->materials[i].volume.attenuation_distance;
			scene.m_Materials.back().m_AttenuationColor = Vector3{
				data->materials[i].volume.attenuation_color[0],
				data->materials[i].volume.attenuation_color[1],
				data->materials[i].volume.attenuation_color[2]
			};
		}
	}

	// Textures (from cgltf_textures)
	for (cgltf_size i = 0; i < data->textures_count; ++i)
	{
		scene.m_Textures.emplace_back();
		const cgltf_texture& tex = data->textures[i];
		if (tex.image)
		{
			scene.m_Textures.back().m_Uri = tex.image->uri ? tex.image->uri : std::string();

			std::filesystem::path uriPath(scene.m_Textures.back().m_Uri);
			std::filesystem::path ddsUri = uriPath;
			ddsUri.replace_extension(".dds");
			std::filesystem::path fullDdsPath = sceneDir / ddsUri;
			if (std::filesystem::exists(fullDdsPath))
			{
				scene.m_Textures.back().m_Uri = ddsUri.string();
			}
		}

		if (tex.sampler)
		{
			const bool isWrap = (tex.sampler->wrap_s == cgltf_wrap_mode_repeat || tex.sampler->wrap_t == cgltf_wrap_mode_repeat);
			scene.m_Textures.back().m_Sampler = isWrap ? Scene::Texture::Wrap : Scene::Texture::Clamp;
		}
		else
		{
			scene.m_Textures.back().m_Sampler = Scene::Texture::Wrap; // Default
		}
	}
}

void SceneLoader::LoadTexturesFromImages(Scene& scene, const std::filesystem::path& sceneDir, Renderer* renderer)
{
	if (Config::Get().m_SkipTextures)
	{
		return;
	}

	SCOPED_TIMER("[Scene] LoadTextures");

	const uint32_t threadCount = renderer->m_TaskScheduler->GetThreadCount() + 1;
	std::vector<nvrhi::CommandListHandle> threadCommandLists(threadCount);
	std::vector<std::unique_ptr<ScopedCommandList>> threadScopedCommandLists(threadCount);
	for (uint32_t i = 0; i < threadCount; ++i)
	{
		threadCommandLists[i] = renderer->AcquireCommandList();
		threadScopedCommandLists[i] = std::make_unique<ScopedCommandList>(threadCommandLists[i], "Texture Load");
	}

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

		desc.debugName = fullPath.string();
		
		tex.m_Handle = renderer->m_RHI->m_NvrhiDevice->createTexture(desc);

		nvrhi::CommandListHandle& cmd = threadCommandLists[threadIndex];
		UploadTexture(cmd, tex.m_Handle, desc, imgData->GetData(), imgData->GetSize());
	});

	threadScopedCommandLists.clear();

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

void SceneLoader::UpdateMaterialsAndCreateConstants(Scene& scene, Renderer* renderer)
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
		mc.m_IOR = mat.m_IOR;
		mc.m_TransmissionFactor = mat.m_TransmissionFactor;
		mc.m_ThicknessFactor = mat.m_ThicknessFactor;
		mc.m_AttenuationDistance = mat.m_AttenuationDistance;
		mc.m_AttenuationColor = mat.m_AttenuationColor;
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

		nvrhi::CommandListHandle cmd = renderer->AcquireCommandList();
		ScopedCommandList scopedCmd{ cmd, "Upload MaterialConstants" };
		scopedCmd->writeBuffer(scene.m_MaterialConstantsBuffer, materialConstants.data(), materialConstants.size() * sizeof(MaterialConstants));
	}
}

void SceneLoader::ProcessCameras(const cgltf_data* data, Scene& scene, const SceneOffsets& offsets)
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

void SceneLoader::ProcessLights(const cgltf_data* data, Scene& scene, const SceneOffsets& offsets)
{
	SCOPED_TIMER("[Scene] Lights");
	bool hasDirectional = false;

	for (cgltf_size i = 0; i < data->lights_count; ++i)
	{
		const cgltf_light& cgLight = data->lights[i];
		if (cgLight.type == cgltf_light_type_point)
			continue;

		Scene::Light light;
		light.m_Name = cgLight.name ? cgLight.name : std::string();
		light.m_Color = Vector3{ cgLight.color[0], cgLight.color[1], cgLight.color[2] };
		light.m_Intensity = cgLight.intensity;
		light.m_Range = cgLight.range;
		light.m_Radius = 0.0f;
		light.m_SpotInnerConeAngle = cgLight.spot_inner_cone_angle;
		light.m_SpotOuterConeAngle = cgLight.spot_outer_cone_angle;
		if (cgLight.type == cgltf_light_type_directional)
		{
			light.m_Type = Scene::Light::Directional;
			hasDirectional = true;
		}
		else if (cgLight.type == cgltf_light_type_spot)
		{
			light.m_Type = Scene::Light::Spot;
		}
		scene.m_Lights.push_back(std::move(light));
	}

	if (!hasDirectional)
	{
		Scene::Light light;
		light.m_Name = "Default Directional";
		light.m_Type = Scene::Light::Directional;
		light.m_Color = Vector3{ 1.0f, 1.0f, 1.0f };
		light.m_Intensity = 1.0f;
		scene.m_Lights.push_back(std::move(light));

		scene.m_Lights.back().m_NodeIndex = (int)scene.m_Nodes.size();
		Scene::Node& lightNode = scene.m_Nodes.emplace_back();
		lightNode.m_LightIndex = 0; // The default directional light will be index 0

		const Vector quat = DirectX::XMQuaternionRotationRollPitchYaw(-scene.m_SunPitch, scene.m_SunYaw, 0.0f);
		DirectX::XMStoreFloat4(&lightNode.m_Rotation, quat);

		const DirectX::XMMATRIX localM = DirectX::XMMatrixRotationQuaternion(DirectX::XMLoadFloat4(&lightNode.m_Rotation));
		DirectX::XMStoreFloat4x4(&lightNode.m_LocalTransform, localM);
		lightNode.m_WorldTransform = lightNode.m_LocalTransform;
	}

	std::sort(scene.m_Lights.begin(), scene.m_Lights.end(), [](const Scene::Light& a, const Scene::Light& b)
	{
		return a.m_Type < b.m_Type; // Directional < Point < Spot
	});
}

void SceneLoader::ProcessAnimations(const cgltf_data* data, Scene& scene, const SceneOffsets& offsets)
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
			int nodeIdx = (int)cgltf_node_index(data, cgChannel.target_node) + offsets.nodeOffset;
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

void SceneLoader::ProcessMeshes(const cgltf_data* data, Scene& scene, std::vector<VertexQuantized>& outVerticesQuantized, std::vector<uint32_t>& outIndices, const SceneOffsets& offsets)
{
	SCOPED_TIMER("[Scene] Meshes");

	struct PrimitiveResult
	{
		std::vector<VertexQuantized> vertices;
		std::vector<uint32_t> indices;
		std::vector<Meshlet> meshlets;
		std::vector<uint32_t> meshletVertices;
		std::vector<uint32_t> meshletTriangles;
		MeshData meshData;
		Scene::Primitive minimalPrim;
	};

	struct MeshResult
	{
		std::vector<PrimitiveResult> primitives;
		std::string name;
	};

	std::vector<MeshResult> meshResults(data->meshes_count);

	struct PrimitiveJob
	{
		uint32_t meshIdx;
		uint32_t primIdx;
		const cgltf_primitive* prim;
	};

	std::vector<PrimitiveJob> jobs;
	for (cgltf_size mi = 0; mi < data->meshes_count; ++mi)
	{
		meshResults[mi].primitives.resize(data->meshes[mi].primitives_count);
		meshResults[mi].name = data->meshes[mi].name ? data->meshes[mi].name : "unnamed";
		for (cgltf_size pi = 0; pi < data->meshes[mi].primitives_count; ++pi)
		{
			jobs.push_back({ (uint32_t)mi, (uint32_t)pi, &data->meshes[mi].primitives[pi] });
		}
	}

	Renderer* renderer = Renderer::GetInstance();
	renderer->m_TaskScheduler->ParallelFor((uint32_t)jobs.size(), [&](uint32_t jobIdx, uint32_t threadIndex)
	{
		const PrimitiveJob& job = jobs[jobIdx];
		const cgltf_primitive& prim = *job.prim;
		PrimitiveResult& res = meshResults[job.meshIdx].primitives[job.primIdx];

		const cgltf_accessor* posAcc = nullptr;
		const cgltf_accessor* normAcc = nullptr;
		const cgltf_accessor* uvAcc = nullptr;
		const cgltf_accessor* tangAcc = nullptr;

		for (cgltf_size ai = 0; ai < prim.attributes_count; ++ai)
		{
			const cgltf_attribute& attr = prim.attributes[ai];
			if (attr.type == cgltf_attribute_type_position)
				posAcc = attr.data;
			else if (attr.type == cgltf_attribute_type_normal)
				normAcc = attr.data;
			else if (attr.type == cgltf_attribute_type_texcoord)
				uvAcc = attr.data;
			else if (attr.type == cgltf_attribute_type_tangent)
				tangAcc = attr.data;
		}

		if (!posAcc)
			return;

		if (!tangAcc)
		{
			if (prim.material)
			{
				int matIdx = offsets.materialOffset + (int)(prim.material - data->materials);
				scene.m_Materials[matIdx].m_NormalTexture = -1;
			}
		}

		const cgltf_size vertCount = posAcc->count;

		std::vector<Vertex> rawVertices(vertCount);
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

			float tang[4] = { 0,0,0,0 };
			if (tangAcc)
			{
				cgltf_size tangComps = cgltf_num_components(tangAcc->type);
				cgltf_accessor_read_float(tangAcc, v, tang, tangComps);
			}
			vx.m_Tangent.x = tang[0]; vx.m_Tangent.y = tang[1]; vx.m_Tangent.z = tang[2]; vx.m_Tangent.w = tang[3];

			rawVertices[v] = vx;
		}

		std::vector<uint32_t> rawIndices;
		if (prim.indices)
		{
			rawIndices.resize(prim.indices->count);
			for (cgltf_size k = 0; k < prim.indices->count; ++k)
			{
				rawIndices[k] = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, k));
			}
		}
		else
		{
			rawIndices.resize(vertCount);
			for (uint32_t k = 0; k < vertCount; ++k) rawIndices[k] = k;
		}

		std::vector<uint32_t> remap(rawIndices.size());
		size_t uniqueVertices = meshopt_generateVertexRemap(remap.data(), rawIndices.data(), rawIndices.size(), rawVertices.data(), rawVertices.size(), sizeof(Vertex));

		std::vector<Vertex> optimizedVertices(uniqueVertices);
		std::vector<uint32_t> localIndices(rawIndices.size());

		meshopt_remapVertexBuffer(optimizedVertices.data(), rawVertices.data(), rawVertices.size(), sizeof(Vertex), remap.data());
		meshopt_remapIndexBuffer(localIndices.data(), rawIndices.data(), rawIndices.size(), remap.data());

		meshopt_optimizeVertexCache(localIndices.data(), localIndices.data(), localIndices.size(), uniqueVertices);
		meshopt_optimizeVertexFetch(optimizedVertices.data(), localIndices.data(), localIndices.size(), optimizedVertices.data(), uniqueVertices, sizeof(Vertex));

		res.vertices.reserve(uniqueVertices);
		for (const Vertex& v : optimizedVertices)
		{
			VertexQuantized vq{};
			vq.m_Pos = v.m_Pos;
			vq.m_Normal = (meshopt_quantizeSnorm(v.m_Normal.x, 10) + 511) |
				((meshopt_quantizeSnorm(v.m_Normal.y, 10) + 511) << 10) |
				((meshopt_quantizeSnorm(v.m_Normal.z, 10) + 511) << 20);
			// bit 30 is bitangent sign (W)
			vq.m_Normal |= (v.m_Tangent.w >= 0 ? 0 : 1) << 30;

			vq.m_Uv = (meshopt_quantizeHalf(v.m_Uv.x)) |
				((meshopt_quantizeHalf(v.m_Uv.y)) << 16);

			// 8-8 octahedral tangent
			float tx = v.m_Tangent.x, ty = v.m_Tangent.y, tz = v.m_Tangent.z;
			float tsum = fabsf(tx) + fabsf(ty) + fabsf(tz);
			if (tsum > 1e-6f)
			{
				float tu = tz >= 0 ? tx / tsum : (1.0f - fabsf(ty / tsum)) * (tx >= 0 ? 1.0f : -1.0f);
				float tv = tz >= 0 ? ty / tsum : (1.0f - fabsf(tx / tsum)) * (ty >= 0 ? 1.0f : -1.0f);
				vq.m_Tangent = (meshopt_quantizeSnorm(tu, 8) + 127) | (meshopt_quantizeSnorm(tv, 8) + 127) << 8;
			}
			else
			{
				vq.m_Tangent = 0;
			}

			res.vertices.push_back(vq);
		}

		uint32_t baseIndexCount = static_cast<uint32_t>(localIndices.size());
		if (baseIndexCount > 0)
		{
			const size_t max_vertices = kMaxMeshletVertices;
			const size_t max_triangles = kMaxMeshletTriangles;
			const float cone_weight = 0.25f;

			const uint32_t kIndexLimitForLODGeneration = 1024;
			const float kIndexReductionPercentageForLODGeneration = 0.5f;
			const size_t kMinimumIndicesForLODGeneration = 128;

			const float target_error_hq = 0.01f;
			const float kMaxErrorForLODGeneration = 0.10f;
			const float kMinReductionRatio = 0.85f;

			const float attribute_weights[3] = { 1.0f, 1.0f, 1.0f };

			const float simplifyScale = meshopt_simplifyScale(&optimizedVertices[0].m_Pos.x, uniqueVertices, sizeof(Vertex));

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
					if (baseIndexCount < kIndexLimitForLODGeneration)
						break;

					size_t target_index_count = size_t(baseIndexCount * pow(kIndexReductionPercentageForLODGeneration, (float)lod));
					target_index_count = std::max(target_index_count, kMinimumIndicesForLODGeneration);

					if (target_index_count >= res.meshData.m_IndexCounts[lod - 1])
						break;

					lodIndices.resize(baseIndexCount);
					size_t new_index_count = meshopt_simplifyWithAttributes(
						lodIndices.data(),
						localIndices.data(), baseIndexCount,
						&optimizedVertices[0].m_Pos.x, uniqueVertices, sizeof(Vertex),
						&optimizedVertices[0].m_Normal.x, sizeof(Vertex),
						attribute_weights, 3,
						nullptr, target_index_count, target_error_hq,
						meshopt_SimplifySparse,
						&lodError);
					lodIndices.resize(new_index_count);

					if (new_index_count < kIndexLimitForLODGeneration)
						break;

					if (new_index_count >= res.meshData.m_IndexCounts[lod - 1] * kMinReductionRatio || lodError > kMaxErrorForLODGeneration)
						break;

					meshopt_optimizeVertexCache(lodIndices.data(), lodIndices.data(), lodIndices.size(), uniqueVertices);
				}

				res.meshData.m_IndexOffsets[lod] = (uint32_t)res.indices.size();
				res.meshData.m_IndexCounts[lod] = (uint32_t)lodIndices.size();
				res.meshData.m_LODErrors[lod] = lodError * simplifyScale;

				for (uint32_t idx : lodIndices)
				{
					res.indices.push_back(idx);
				}

				size_t max_meshlets = meshopt_buildMeshletsBound(lodIndices.size(), max_vertices, max_triangles);
				std::vector<meshopt_Meshlet> localMeshlets(max_meshlets);
				std::vector<unsigned int> meshlet_vertices(max_meshlets * max_vertices);
				std::vector<unsigned char> meshlet_triangles(max_meshlets * max_triangles * 3);

				const size_t meshlet_count = meshopt_buildMeshlets(localMeshlets.data(), meshlet_vertices.data(), meshlet_triangles.data(),
					lodIndices.data(), lodIndices.size(), &optimizedVertices[0].m_Pos.x, uniqueVertices, sizeof(Vertex),
					max_vertices, max_triangles, cone_weight);

				localMeshlets.resize(meshlet_count);

				res.meshData.m_MeshletOffsets[lod] = (uint32_t)res.meshlets.size();
				res.meshData.m_MeshletCounts[lod] = (uint32_t)meshlet_count;
				res.meshData.m_LODCount = lod + 1;

				for (size_t i = 0; i < meshlet_count; ++i)
				{
					const meshopt_Meshlet& m = localMeshlets[i];
					meshopt_optimizeMeshlet(&meshlet_vertices[m.vertex_offset], &meshlet_triangles[m.triangle_offset], m.triangle_count, m.vertex_count);

					meshopt_Bounds bounds = meshopt_computeMeshletBounds(&meshlet_vertices[m.vertex_offset], &meshlet_triangles[m.triangle_offset],
						m.triangle_count, &optimizedVertices[0].m_Pos.x, uniqueVertices, sizeof(Vertex));

					Meshlet gpuMeshlet;
					gpuMeshlet.m_VertexOffset = (uint32_t)(res.meshletVertices.size());
					gpuMeshlet.m_TriangleOffset = (uint32_t)(res.meshletTriangles.size());
					gpuMeshlet.m_VertexCount = (uint32_t)m.vertex_count;
					gpuMeshlet.m_TriangleCount = (uint32_t)m.triangle_count;

					gpuMeshlet.m_CenterRadius[0] = meshopt_quantizeHalf(bounds.center[0]) | (meshopt_quantizeHalf(bounds.center[1]) << 16);
					gpuMeshlet.m_CenterRadius[1] = meshopt_quantizeHalf(bounds.center[2]) | (meshopt_quantizeHalf(bounds.radius) << 16);

					const uint32_t packedAxisX = (uint32_t)((bounds.cone_axis[0] + 1.0f) * 0.5f * UINT8_MAX);
					const uint32_t packedAxisY = (uint32_t)((bounds.cone_axis[1] + 1.0f) * 0.5f * UINT8_MAX);
					const uint32_t packedAxisZ = (uint32_t)((bounds.cone_axis[2] + 1.0f) * 0.5f * UINT8_MAX);
					const uint32_t packedCutoff = (uint32_t)(bounds.cone_cutoff_s8 * 2);

					gpuMeshlet.m_ConeAxisAndCutoff = packedAxisX | (packedAxisY << 8) | (packedAxisZ << 16) | (packedCutoff << 24);

					for (uint32_t v = 0; v < m.vertex_count; ++v)
					{
						res.meshletVertices.push_back(meshlet_vertices[m.vertex_offset + v]);
					}

					for (uint32_t t = 0; t < m.triangle_count; ++t)
					{
						uint32_t i0 = meshlet_triangles[m.triangle_offset + t * 3 + 0];
						uint32_t i1 = meshlet_triangles[m.triangle_offset + t * 3 + 1];
						uint32_t i2 = meshlet_triangles[m.triangle_offset + t * 3 + 2];
						res.meshletTriangles.push_back(i0 | (i1 << 8) | (i2 << 16));
					}

					res.meshlets.push_back(gpuMeshlet);
				}
			}
		}

		res.minimalPrim.m_VertexCount = (uint32_t)uniqueVertices;
		res.minimalPrim.m_MaterialIndex = prim.material ? static_cast<int>(cgltf_material_index(data, prim.material)) + offsets.materialOffset : -1;

		SDL_Log("[Scene] Processed Mesh %u Primitive %u: %zu vertices, %zu indices, %zu meshlets",
			job.meshIdx, job.primIdx, res.vertices.size(), res.indices.size(), res.meshlets.size());
	});

	// Merging results
	uint32_t currentVertexOffset = (uint32_t)outVerticesQuantized.size();
	uint32_t currentIndexOffset = (uint32_t)outIndices.size();
	uint32_t currentMeshletOffset = (uint32_t)scene.m_Meshlets.size();
	uint32_t currentMeshletVertexOffset = (uint32_t)scene.m_MeshletVertices.size();
	uint32_t currentMeshletTriangleOffset = (uint32_t)scene.m_MeshletTriangles.size();
	uint32_t currentMeshDataOffset = (uint32_t)scene.m_MeshData.size();

	for (uint32_t mi = 0; mi < (uint32_t)meshResults.size(); ++mi)
	{
		MeshResult& meshRes = meshResults[mi];
		Scene::Mesh mesh;
		uint32_t meshFirstVertex = currentVertexOffset;

		for (uint32_t pi = 0; pi < (uint32_t)meshRes.primitives.size(); ++pi)
		{
			PrimitiveResult& primRes = meshRes.primitives[pi];

			primRes.minimalPrim.m_VertexOffset = currentVertexOffset;
			primRes.minimalPrim.m_MeshDataIndex = currentMeshDataOffset;
			mesh.m_Primitives.push_back(primRes.minimalPrim);

			for (uint32_t& idx : primRes.indices)
			{
				idx += currentVertexOffset;
			}

			for (uint32_t lod = 0; lod < primRes.meshData.m_LODCount; ++lod)
			{
				primRes.meshData.m_IndexOffsets[lod] += currentIndexOffset;
				primRes.meshData.m_MeshletOffsets[lod] += currentMeshletOffset;
			}
			scene.m_MeshData.push_back(primRes.meshData);

			for (Meshlet& m : primRes.meshlets)
			{
				m.m_VertexOffset += currentMeshletVertexOffset;
				m.m_TriangleOffset += currentMeshletTriangleOffset;
				scene.m_Meshlets.push_back(m);
			}

			for (uint32_t& v : primRes.meshletVertices)
			{
				scene.m_MeshletVertices.push_back(v + currentVertexOffset);
			}

			for (uint32_t& t : primRes.meshletTriangles)
			{
				scene.m_MeshletTriangles.push_back(t);
			}

			outVerticesQuantized.insert(outVerticesQuantized.end(), primRes.vertices.begin(), primRes.vertices.end());
			outIndices.insert(outIndices.end(), primRes.indices.begin(), primRes.indices.end());

			currentVertexOffset += (uint32_t)primRes.vertices.size();
			currentIndexOffset += (uint32_t)primRes.indices.size();
			currentMeshletOffset += (uint32_t)primRes.meshlets.size();
			currentMeshletVertexOffset += (uint32_t)primRes.meshletVertices.size();
			currentMeshletTriangleOffset += (uint32_t)primRes.meshletTriangles.size();
			currentMeshDataOffset++;
		}

		Sphere s;
		if (!mesh.m_Primitives.empty())
		{
			uint32_t meshVertexCount = currentVertexOffset - meshFirstVertex;
			Sphere::CreateFromPoints(s, meshVertexCount, &outVerticesQuantized[meshFirstVertex].m_Pos, sizeof(VertexQuantized));
		}
		else
		{
			s.Center = { 0,0,0 };
			s.Radius = 0;
		}
		mesh.m_Center = (Vector3)s.Center;
		mesh.m_Radius = s.Radius;

		SDL_Log("[Scene] Mesh %u [%s]: %zu primitives", mi, meshRes.name.c_str(), meshRes.primitives.size());
		scene.m_Meshes.push_back(std::move(mesh));
	}

	SDL_Log("[Scene] ProcessMeshes completed:\n"
		"  Vertices (Quant):  %zu\n"
		"  Indices:           %zu\n"
		"  Meshlets:          %zu\n"
		"  Meshlet Vertices:  %zu\n"
		"  Meshlet Triangles: %zu",
		outVerticesQuantized.size(), outIndices.size(), scene.m_Meshlets.size(), 
		scene.m_MeshletVertices.size(), scene.m_MeshletTriangles.size() / 3);
}

void SceneLoader::ProcessNodesAndHierarchy(const cgltf_data* data, Scene& scene, const SceneOffsets& offsets)
{
	SCOPED_TIMER("[Scene] Nodes+Hierarchy");
	std::unordered_map<cgltf_size, int> nodeMap;
	for (cgltf_size ni = 0; ni < data->nodes_count; ++ni)
	{
		const cgltf_node& cn = data->nodes[ni];
		Scene::Node& node = scene.m_Nodes[ni + offsets.nodeOffset];
		node.m_Name = cn.name ? cn.name : std::string();
		node.m_MeshIndex = cn.mesh ? static_cast<int>(cgltf_mesh_index(data, cn.mesh)) + offsets.meshOffset : -1;
		node.m_CameraIndex = cn.camera ? static_cast<int>(cgltf_camera_index(data, cn.camera)) + offsets.cameraOffset : -1;
		node.m_LightIndex = cn.light ? static_cast<int>(cgltf_light_index(data, cn.light)) + offsets.lightOffset : -1;

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
		nodeMap[nodeIndex] = static_cast<int>(ni) + offsets.nodeOffset;
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
	for (size_t i = 0; i < data->nodes_count; ++i)
	{
		const Scene::Node& node = scene.m_Nodes[i + offsets.nodeOffset];
		if (node.m_CameraIndex >= 0 && node.m_CameraIndex < static_cast<int>(scene.m_Cameras.size()))
		{
			scene.m_Cameras[node.m_CameraIndex].m_NodeIndex = static_cast<int>(i) + offsets.nodeOffset;
		}
		if (node.m_LightIndex >= 0 && node.m_LightIndex < static_cast<int>(scene.m_Lights.size()))
		{
			scene.m_Lights[node.m_LightIndex].m_NodeIndex = static_cast<int>(i) + offsets.nodeOffset;
		}
	}

	// Compute world transforms
	for (size_t i = 0; i < data->nodes_count; ++i)
	{
		if (scene.m_Nodes[i + offsets.nodeOffset].m_Parent == -1)
		{
			Matrix identity{};
			DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
			ComputeWorldTransforms(scene, static_cast<int>(i) + offsets.nodeOffset, identity);
		}
	}

	// Compute per-node bounding spheres by transforming mesh spheres into world space
	for (size_t ni = 0; ni < data->nodes_count; ++ni)
	{
		scene.UpdateNodeBoundingSphere(static_cast<int>(ni) + offsets.nodeOffset);
	}
}

void SceneLoader::CreateAndUploadGpuBuffers(Scene& scene, Renderer* renderer, const std::vector<VertexQuantized>& allVerticesQuantized, const std::vector<uint32_t>& allIndices)
{
	SCOPED_TIMER("[Scene] GPU Upload");

	nvrhi::CommandListHandle cmd = renderer->AcquireCommandList();
	ScopedCommandList scopedCmd{ cmd, "Upload Scene Buffers" };

	// Create quantized vertex buffer
	if (!allVerticesQuantized.empty())
	{
		size_t vqbytes = allVerticesQuantized.size() * sizeof(VertexQuantized);
		nvrhi::BufferDesc desc{};
		desc.byteSize = (uint32_t)vqbytes;
		desc.structStride = sizeof(VertexQuantized);
		desc.isVertexBuffer = true;
		desc.isAccelStructBuildInput = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "Scene_VertexBufferQuantized";
		scene.m_VertexBufferQuantized = renderer->m_RHI->m_NvrhiDevice->createBuffer(desc);

		cmd->writeBuffer(scene.m_VertexBufferQuantized, allVerticesQuantized.data(), vqbytes, 0);
	}

	// Create index buffer
	if (!allIndices.empty())
	{
		size_t ibytes = allIndices.size() * sizeof(uint32_t);
		nvrhi::BufferDesc desc{};
		desc.byteSize = (uint32_t)ibytes;
		desc.structStride = sizeof(uint32_t);
		desc.isIndexBuffer = true;
		desc.initialState = nvrhi::ResourceStates::IndexBuffer;
		desc.keepInitialState = true;
		desc.debugName = "Scene_IndexBuffer";
		desc.isAccelStructBuildInput = true;
		scene.m_IndexBuffer = renderer->m_RHI->m_NvrhiDevice->createBuffer(desc);

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
		if (scene.m_InstanceDataBuffer && !scene.m_InstanceData.empty())
			cmd->writeBuffer(scene.m_InstanceDataBuffer, scene.m_InstanceData.data(), scene.m_InstanceData.size() * sizeof(PerInstanceData), 0);
	}

	// print buffers memory stats
	SDL_Log("[Scene] GPU Buffers Uploaded:\n"
		"  Vertex Buffer (Quant): %.2f MB (%zu vertices)\n"
		"  Index Buffer:          %.2f MB (%zu indices)\n"
		"  Mesh Data Buffer:      %.2f MB (%zu mesh data entries)\n"
		"  Meshlet Buffer:        %.2f MB (%zu meshlets)\n"
		"  Meshlet Vertices Buf:  %.2f MB (%zu meshlet vertices)\n"
		"  Meshlet Triangles Buf: %.2f MB (%zu meshlet triangles)\n"
		"  Instance Data Buffer:  %.2f MB (%zu instances)",
		(allVerticesQuantized.size() * sizeof(VertexQuantized)) / (1024.0f * 1024.0f), allVerticesQuantized.size(),
		(allIndices.size() * sizeof(uint32_t)) / (1024.0f * 1024.0f), allIndices.size(),
		(scene.m_MeshData.size() * sizeof(MeshData)) / (1024.0f * 1024.0f), scene.m_MeshData.size(),
		(scene.m_Meshlets.size() * sizeof(Meshlet)) / (1024.0f * 1024.0f), scene.m_Meshlets.size(),
		(scene.m_MeshletVertices.size() * sizeof(uint32_t)) / (1024.0f * 1024.0f), scene.m_MeshletVertices.size(),
		(scene.m_MeshletTriangles.size() * sizeof(uint32_t)) / (1024.0f * 1024.0f), scene.m_MeshletTriangles.size(),
		(scene.m_InstanceData.size() * sizeof(PerInstanceData)) / (1024.0f * 1024.0f), scene.m_InstanceData.size());
}

void SceneLoader::CreateAndUploadLightBuffer(Scene& scene, Renderer* renderer)
{
	std::vector<GPULight> gpuLights;

	for (const Scene::Light& light : scene.m_Lights)
	{
		GPULight gl;
		gl.m_Type = (uint32_t)light.m_Type;
		gl.m_Color = light.m_Color;
		gl.m_Intensity = light.m_Intensity;
		gl.m_Range = light.m_Range;
		gl.m_Radius = light.m_Radius;
		gl.m_SpotInnerConeAngle = light.m_SpotInnerConeAngle;
		gl.m_SpotOuterConeAngle = light.m_SpotOuterConeAngle;
		gl.pad0 = 0;

		if (light.m_NodeIndex != -1)
		{
			const Scene::Node& node = scene.m_Nodes[light.m_NodeIndex];
			DirectX::XMMATRIX worldM = DirectX::XMLoadFloat4x4(&node.m_WorldTransform);

			DirectX::XMVECTOR worldPosVec, worldScaleVec, worldRotVec;
			DirectX::XMMatrixDecompose(&worldScaleVec, &worldRotVec, &worldPosVec, worldM);

			DirectX::XMStoreFloat3(&gl.m_Position, worldPosVec);

			DirectX::XMVECTOR localDirVec = DirectX::XMVectorSet(0, 0, -1, 0); // GLTF forward is -Z
			DirectX::XMVECTOR worldDirVec = DirectX::XMVector3TransformNormal(localDirVec, worldM);
			worldDirVec = DirectX::XMVector3Normalize(worldDirVec);
			DirectX::XMStoreFloat3(&gl.m_Direction, worldDirVec);
		}
		else
		{
			gl.m_Position = Vector3(0, 0, 0);
			gl.m_Direction = Vector3(0, -1, 0);
		}

		gpuLights.push_back(gl);
	}

	scene.m_LightCount = (uint32_t)gpuLights.size();

	if (!gpuLights.empty())
	{
		bool needsNewBuffer = !scene.m_LightBuffer || (scene.m_LightBuffer->getDesc().byteSize < (uint32_t)(gpuLights.size() * sizeof(GPULight)));

		if (needsNewBuffer)
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = (uint32_t)(gpuLights.size() * sizeof(GPULight));
			desc.debugName = "LightBuffer";
			desc.structStride = sizeof(GPULight);
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.canHaveUAVs = true; // For potential future compute sorting

			scene.m_LightBuffer = renderer->m_RHI->m_NvrhiDevice->createBuffer(desc);
		}

		nvrhi::CommandListHandle cmd = renderer->AcquireCommandList();
		ScopedCommandList scopedCmd(cmd, "Upload Light Buffer");
		cmd->writeBuffer(scene.m_LightBuffer, gpuLights.data(), (uint32_t)(gpuLights.size() * sizeof(GPULight)));
	}
}

bool SceneLoader::LoadGLTFScene(Scene& scene, const std::string& scenePath, std::vector<VertexQuantized>& allVerticesQuantized, std::vector<uint32_t>& allIndices)
{
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

	const std::filesystem::path sceneDir = std::filesystem::path(scenePath).parent_path();

	SceneOffsets offsets;
	offsets.nodeOffset = (int)scene.m_Nodes.size();
	offsets.meshOffset = (int)scene.m_Meshes.size();
	offsets.materialOffset = (int)scene.m_Materials.size();
	offsets.textureOffset = (int)scene.m_Textures.size();
	offsets.cameraOffset = (int)scene.m_Cameras.size();
	offsets.lightOffset = (int)scene.m_Lights.size();

	scene.m_Nodes.resize(offsets.nodeOffset + data->nodes_count);
	ProcessMaterialsAndImages(data, scene, sceneDir, offsets);
	ProcessCameras(data, scene, offsets);
	ProcessLights(data, scene, offsets);
	ProcessAnimations(data, scene, offsets);
	ProcessMeshes(data, scene, allVerticesQuantized, allIndices, offsets);
	ProcessNodesAndHierarchy(data, scene, offsets);

	cgltf_free(data);
	return true;
}