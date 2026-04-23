#include "Scene.h"
#include "SceneLoader.h"
#include "Config.h"
#include "Renderer.h"
#include "CommonResources.h"
#include "Utilities.h"

void Scene::LoadScene()
{
	const std::string& scenePath = Config::Get().m_ScenePath;
	if (scenePath.empty())
	{
		SDL_Log("[Scene] No scene configured, skipping load");
		return;
	}

	const std::filesystem::path sceneFilePath(scenePath);
	const std::filesystem::path sceneDir = sceneFilePath.parent_path();

	
	std::vector<srrhi::VertexQuantized> allVerticesQuantized;
	std::vector<uint32_t> allIndices;

	//SCOPED_TIMER("[Scene] LoadScene Total");

	bool loadedFromCache = false;
	const std::string filename = sceneFilePath.filename().string();
	const bool bIsSceneJson = filename.size() >= 11 && filename.substr(filename.size() - 11) == ".scene.json";

	if (!Config::Get().m_SkipCache)
	{
		const std::filesystem::path cachePath = sceneFilePath.parent_path() / (sceneFilePath.stem().string() + "_cooked.bin");
		if (std::filesystem::exists(cachePath))
		{
			const std::filesystem::file_time_type sceneTime = std::filesystem::last_write_time(sceneFilePath);
			const std::filesystem::file_time_type cacheTime = std::filesystem::last_write_time(cachePath);
			if (cacheTime > sceneTime)
			{
				SDL_Log("[Scene] Loading from binary cache: %s", cachePath.string().c_str());
				if (LoadFromCache(cachePath.string(), allIndices, allVerticesQuantized))
				{
					loadedFromCache = true;
				}
				else
				{
					SDL_Log("[Scene] Cache load failed, falling back to scene loading");
				}
			}
		}
	}

	if (!loadedFromCache)
	{
		bool success = false;
		if (bIsSceneJson)
		{
			success = SceneLoader::LoadJSONScene(*this, scenePath, allVerticesQuantized, allIndices);
		}
		else
		{
			const bool bFromJSONScene = false;
			success = SceneLoader::LoadGLTFScene(*this, scenePath, allVerticesQuantized, allIndices, bFromJSONScene);
		}

		if (!success)
		{
			SDL_LOG_ASSERT_FAIL("Scene load failed", "[Scene] Failed to load scene: %s", scenePath.c_str());
		}

		if (!Config::Get().m_SkipCache)
		{
			const std::filesystem::path cachePath = sceneFilePath.parent_path() / (sceneFilePath.stem().string() + "_cooked.bin");
			SDL_Log("[Scene] Saving binary cache: %s", cachePath.string().c_str());
			SaveToCache(cachePath.string(), allIndices, allVerticesQuantized);
		}
	}

	FinalizeLoadedScene();

	SceneLoader::LoadTexturesFromImages(*this, sceneDir);
	SceneLoader::UpdateMaterialsAndCreateConstants(*this);
	SceneLoader::CreateAndUploadGpuBuffers(*this, allVerticesQuantized, allIndices);
	SceneLoader::CreateAndUploadLightBuffer(*this);
	BuildAccelerationStructures();

	if (!m_Cameras.empty())
	{
		const Scene::Camera& firstCam = m_Cameras[0];
		g_Renderer.SetCameraFromSceneCamera(firstCam);
		m_SelectedCameraIndex = 0;
	}
}

void Scene::BuildAccelerationStructures()
{
	//SCOPED_TIMER("[Scene] Build Accel Structs");

    nvrhi::IDevice* device = g_Renderer.m_RHI->m_NvrhiDevice;
	nvrhi::CommandListHandle cmd = g_Renderer.AcquireCommandList();
	ScopedCommandList scopedCmd{ cmd, "Build Scene Accel Structs" };

    // Mapping from MeshDataIndex to Primitive pointer for TLAS build
    std::vector<Primitive*> meshDataToPrimitive(m_MeshData.size(), nullptr);

	// 1. Build one BLAS per LOD level per primitive
	uint64_t totalBLASMemoryBytes = 0;
	for (Mesh& mesh : m_Meshes)
	{
		for (Primitive& primitive : mesh.m_Primitives)
		{
			SDL_assert(primitive.m_BLAS.empty());
			meshDataToPrimitive[primitive.m_MeshDataIndex] = &primitive;

			const srrhi::MeshData& meshData = m_MeshData[primitive.m_MeshDataIndex];
			const uint32_t lodCount = meshData.m_LODCount;
			SDL_assert(lodCount > 0 && lodCount <= srrhi::CommonConsts::MAX_LOD_COUNT);

			primitive.m_BLAS.resize(lodCount);

			for (uint32_t lod = 0; lod < lodCount; ++lod)
			{
				nvrhi::rt::GeometryDesc geometryDesc;
				nvrhi::rt::GeometryTriangles& geometryTriangle = geometryDesc.geometryData.triangles;
				geometryTriangle.indexBuffer = m_IndexBuffer;
				geometryTriangle.vertexBuffer = m_VertexBufferQuantized;
				geometryTriangle.indexFormat = nvrhi::Format::R32_UINT;
				geometryTriangle.vertexFormat = nvrhi::Format::RGB32_FLOAT;
				geometryTriangle.indexOffset = meshData.m_IndexOffsets[lod] * nvrhi::getFormatInfo(geometryTriangle.indexFormat).bytesPerBlock;
				geometryTriangle.vertexOffset = 0; // Indices are already global relative to the start of the vertex buffer
				geometryTriangle.indexCount = meshData.m_IndexCounts[lod];
				geometryTriangle.vertexCount = primitive.m_VertexCount;
				geometryTriangle.vertexStride = sizeof(srrhi::VertexQuantized);

				geometryDesc.flags = nvrhi::rt::GeometryFlags::None; // can't be opaque since we have alpha tested materials that can be applied to this mesh
				geometryDesc.geometryType = nvrhi::rt::GeometryType::Triangles;

				nvrhi::rt::AccelStructDesc blasDesc;
				blasDesc.bottomLevelGeometries = { geometryDesc };
				blasDesc.debugName = (std::string("BLAS_LOD") + std::to_string(lod)).c_str();
				blasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastTrace;

				primitive.m_BLAS[lod] = device->createAccelStruct(blasDesc);
				nvrhi::utils::BuildBottomLevelAccelStruct(scopedCmd, primitive.m_BLAS[lod], blasDesc);

				// Accumulate memory for logging (Req 7)
				totalBLASMemoryBytes += device->getAccelStructMemoryRequirements(primitive.m_BLAS[lod]).size;
			}
		}
	}

	//SDL_Log("[Scene] Total BLAS memory across all LODs: %.2f MB", totalBLASMemoryBytes / (1024.0 * 1024.0));

	// 2. Build TLAS for the scene
	nvrhi::rt::AccelStructDesc tlasDesc;
    tlasDesc.topLevelMaxInstances =  (uint32_t)m_InstanceData.size();
    tlasDesc.debugName = "Scene TLAS";
    tlasDesc.isTopLevel = true;
    m_TLAS = device->createAccelStruct(tlasDesc);

	SDL_assert(m_RTInstanceDescs.empty());
	for (uint32_t instanceID = 0; instanceID < m_InstanceData.size(); ++instanceID)
    {
		const srrhi::PerInstanceData& instData = m_InstanceData[instanceID];
		Primitive* primitive = meshDataToPrimitive.at(instData.m_MeshDataIndex);
		const uint32_t alphaMode = primitive->m_MaterialIndex != -1 ? m_Materials.at(primitive->m_MaterialIndex).m_AlphaMode : srrhi::CommonConsts::ALPHA_MODE_OPAQUE;

        nvrhi::rt::InstanceDesc& instanceDesc = m_RTInstanceDescs.emplace_back();

		// Copy transform (transpose of row-vector matrix)
		const Matrix& world = instData.m_World;
		nvrhi::rt::AffineTransform transform;
		transform[0] = world._11; transform[1] = world._21; transform[2] = world._31; transform[3] = world._41;
		transform[4] = world._12; transform[5] = world._22; transform[6] = world._32; transform[7] = world._42;
		transform[8] = world._13; transform[9] = world._23; transform[10] = world._33; transform[11] = world._43;
		instanceDesc.setTransform(transform);

        nvrhi::rt::InstanceFlags instanceFlags = nvrhi::rt::InstanceFlags::None;
        instanceFlags = instanceFlags | ((alphaMode == srrhi::CommonConsts::ALPHA_MODE_OPAQUE) ? nvrhi::rt::InstanceFlags::ForceOpaque : nvrhi::rt::InstanceFlags::ForceNonOpaque);

        instanceDesc.instanceID = instanceID;
        instanceDesc.instanceMask = 1;
        instanceDesc.instanceContributionToHitGroupIndex = 0;
        instanceDesc.flags = instanceFlags;
        // Default to LOD 0 BLAS; TLASPatch_CS will overwrite with the correct LOD address each frame.
        instanceDesc.blasDeviceAddress = primitive->m_BLAS[0]->getDeviceAddress();
    }

    // Create RT instance desc buffer
    if (!m_RTInstanceDescs.empty())
    {
        nvrhi::BufferDesc rtInstDesc;
        rtInstDesc.byteSize = (uint32_t)(m_RTInstanceDescs.size() * sizeof(nvrhi::rt::InstanceDesc));
		rtInstDesc.structStride = sizeof(nvrhi::rt::InstanceDesc);
        rtInstDesc.debugName = "RTInstanceDescBuffer";
        rtInstDesc.isAccelStructBuildInput = true;
        rtInstDesc.canHaveUAVs = true; // TLASPatch_CS writes into this buffer
        rtInstDesc.initialState = nvrhi::ResourceStates::AccelStructBuildInput;
        rtInstDesc.keepInitialState = true;
        m_RTInstanceDescBuffer = device->createBuffer(rtInstDesc);

        // Initial upload
        scopedCmd->writeBuffer(m_RTInstanceDescBuffer, m_RTInstanceDescs.data(), rtInstDesc.byteSize);

        scopedCmd->buildTopLevelAccelStructFromBuffer(m_TLAS, m_RTInstanceDescBuffer, 0, (uint32_t)m_RTInstanceDescs.size());
    }

    // 3. Build the flat BLAS address buffer: blasAddresses[instanceIndex * srrhi::CommonConsts::MAX_LOD_COUNT + lodIndex]
    //    This is uploaded once at scene load and read by TLASPatch_CS each frame.
    {
        const uint32_t numInstances = (uint32_t)m_InstanceData.size();
        const uint32_t totalEntries = numInstances * srrhi::CommonConsts::MAX_LOD_COUNT;
        std::vector<uint64_t> blasAddresses(totalEntries, 0);

        for (uint32_t instanceID = 0; instanceID < numInstances; ++instanceID)
        {
            const srrhi::PerInstanceData& instData = m_InstanceData[instanceID];
            Primitive* primitive = meshDataToPrimitive.at(instData.m_MeshDataIndex);
            const uint32_t lodCount = (uint32_t)primitive->m_BLAS.size();

            for (uint32_t lod = 0; lod < srrhi::CommonConsts::MAX_LOD_COUNT; ++lod)
            {
                // Clamp to highest available LOD to avoid null/invalid addresses (Req 2 AC3)
                const uint32_t clampedLod = (lod < lodCount) ? lod : (lodCount - 1);
                blasAddresses[instanceID * srrhi::CommonConsts::MAX_LOD_COUNT + lod] = primitive->m_BLAS[clampedLod]->getDeviceAddress();
            }
        }

        nvrhi::BufferDesc blasAddrDesc;
        blasAddrDesc.byteSize = totalEntries * sizeof(uint64_t);
        blasAddrDesc.structStride = sizeof(uint64_t);
        blasAddrDesc.debugName = "BLASAddressBuffer";
        blasAddrDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        blasAddrDesc.keepInitialState = true;
        m_BLASAddressBuffer = device->createBuffer(blasAddrDesc);

        if (totalEntries > 0)
        {
            scopedCmd->writeBuffer(m_BLASAddressBuffer, blasAddresses.data(), blasAddrDesc.byteSize);
        }
    }
}

void Scene::FinalizeLoadedScene()
{
    //SCOPED_TIMER("[Scene] Finalize Scene");

    // 1. Identify dynamic nodes and sort them topologically
    // Mark nodes targeted by animations as animated before the dynamic pass.
    // Also collect dynamic material indices for emissive intensity animations.
    m_DynamicMaterialIndices.clear();
    for (const Animation& anim : m_Animations)
    {
        for (const AnimationChannel& chan : anim.m_Channels)
        {
            // Primary single-node target (glTF) and multi-node targets (JSON)
            for (int ni : chan.m_NodeIndices)
            {
                if (ni >= 0 && ni < (int)m_Nodes.size())
                    m_Nodes[ni].m_IsAnimated = true;
                else
                    SDL_Log("[Scene] FinalizeLoadedScene: animation '%s' has out-of-range node index %d (node count=%d)",
                        anim.m_Name.c_str(), ni, (int)m_Nodes.size());
            }
            // Material targets (EmissiveIntensity)
            for (int mi : chan.m_MaterialIndices)
            {
                if (mi >= 0 && mi < (int)m_Materials.size())
                    m_DynamicMaterialIndices.push_back(mi);
                else
                    SDL_Log("[Scene] FinalizeLoadedScene: animation '%s' has out-of-range material index %d (material count=%d)",
                        anim.m_Name.c_str(), mi, (int)m_Materials.size());
            }
        }
    }
    // Deduplicate dynamic material indices
    std::sort(m_DynamicMaterialIndices.begin(), m_DynamicMaterialIndices.end());
    m_DynamicMaterialIndices.erase(std::unique(m_DynamicMaterialIndices.begin(), m_DynamicMaterialIndices.end()), m_DynamicMaterialIndices.end());

    m_DynamicNodeIndices.clear();
    std::function<void(int, bool)> IdentifyDynamic = [&](int idx, bool parentDynamic)
    {
        Node& node = m_Nodes[idx];
        node.m_IsDynamic = node.m_IsAnimated || node.m_LightIndex != -1 || parentDynamic;
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

    // 2. Bucketize and fill instance data
    m_InstanceData.clear();
    struct InstInfo { srrhi::PerInstanceData data; int nodeIdx; };
    std::vector<InstInfo> opaqueStatic, opaqueDynamic;
    std::vector<InstInfo> maskedStatic, maskedDynamic;
    std::vector<InstInfo> transparentStatic, transparentDynamic;

    for (int ni = 0; ni < (int)m_Nodes.size(); ++ni)
    {
        const Node& node = m_Nodes[ni];
        if (node.m_MeshIndex < 0) continue;
        const Mesh& mesh = m_Meshes[node.m_MeshIndex];
        for (const Primitive& prim : mesh.m_Primitives)
        {
            srrhi::PerInstanceData inst{};
            inst.m_World = node.m_WorldTransform;
            inst.m_PrevWorld = node.m_WorldTransform;
            inst.m_MaterialIndex = prim.m_MaterialIndex;
            inst.m_MeshDataIndex = prim.m_MeshDataIndex;
            inst.m_Center = node.m_Center;
            inst.m_Radius = node.m_Radius;

            uint32_t alphaMode = prim.m_MaterialIndex >= 0 ? m_Materials[prim.m_MaterialIndex].m_AlphaMode : srrhi::CommonConsts::ALPHA_MODE_OPAQUE;
            bool isDynamic = node.m_IsDynamic;

            if (alphaMode == srrhi::CommonConsts::ALPHA_MODE_OPAQUE) {
                if (isDynamic) opaqueDynamic.push_back({ inst, ni });
                else opaqueStatic.push_back({ inst, ni });
            }
            else if (alphaMode == srrhi::CommonConsts::ALPHA_MODE_MASK) {
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

	m_SceneBoundingSphere = DirectX::BoundingSphere{};
	for (const Scene::Node& node : m_Nodes)
	{
		const DirectX::BoundingSphere nodeSphere(node.m_Center, node.m_Radius);
		DirectX::BoundingSphere::CreateMerged(m_SceneBoundingSphere, m_SceneBoundingSphere, nodeSphere);
	}

    //SDL_Log("[Scene] Finalized: Instances: Opaque: %u, Masked: %u, Transparent: %u", m_OpaqueBucket.m_Count, m_MaskedBucket.m_Count, m_TransparentBucket.m_Count);
}

// ─── Animation evaluation helper ─────────────────────────────────────────────

// Evaluate an AnimationSampler at time t.
// Returns a Vector4; for scalar attributes only .x is meaningful.
static Vector4 EvaluateAnimSampler(const Scene::AnimationSampler& sampler, float t)
{
    const auto& inputs  = sampler.m_Inputs;
    const auto& outputs = sampler.m_Outputs;

    if (inputs.empty()) return Vector4{ 0,0,0,1 };
    if (inputs.size() == 1) return outputs[0];

    // Clamp to authored range
    if (t <= inputs.front()) return outputs.front();
    if (t >= inputs.back())  return outputs.back();

    // Find surrounding keyframe pair
    uint32_t k0 = 0;
    for (uint32_t i = 0; i < (uint32_t)inputs.size() - 1; ++i)
    {
        if (t >= inputs[i]) k0 = i;
    }
    uint32_t k1 = k0 + 1;

    float dt    = inputs[k1] - inputs[k0];
    float alpha = (dt > 0.0f) ? (t - inputs[k0]) / dt : 0.0f;

    using namespace DirectX;
    XMVECTOR v0 = XMLoadFloat4(&outputs[k0]);
    XMVECTOR v1 = XMLoadFloat4(&outputs[k1]);

    XMVECTOR result;
    switch (sampler.m_Interpolation)
    {
    case Scene::AnimationSampler::Interpolation::Step:
        result = v0;
        break;

    case Scene::AnimationSampler::Interpolation::Slerp:
        result = XMQuaternionSlerp(XMQuaternionNormalize(v0), XMQuaternionNormalize(v1), alpha);
        break;

    case Scene::AnimationSampler::Interpolation::CatmullRom:
    {
        uint32_t km1 = (k0 > 0) ? k0 - 1 : k0;
        uint32_t k2  = (k1 < (uint32_t)inputs.size() - 1) ? k1 + 1 : k1;
        XMVECTOR vm1 = XMLoadFloat4(&outputs[km1]);
        XMVECTOR v2  = XMLoadFloat4(&outputs[k2]);
        result = XMVectorCatmullRom(vm1, v0, v1, v2, alpha);
        break;
    }

    case Scene::AnimationSampler::Interpolation::Linear:
    case Scene::AnimationSampler::Interpolation::CubicSpline:
    default:
        result = XMVectorLerp(v0, v1, alpha);
        break;
    }

    Vector4 out;
    XMStoreFloat4(&out, result);
    return out;
}

void Scene::Update(float deltaTime)
{
	PROFILE_FUNCTION();

	// Save current worlds as previous worlds for all instances (always, for motion vectors).
	for (srrhi::PerInstanceData& inst : m_InstanceData)
	{
		inst.m_PrevWorld = inst.m_World;
	}

	// Respect the global animations toggle.  The Renderer already gates this call,
	// but Scene::Update is also called directly from tests and tools, so the guard
	// lives here as well to ensure consistent behaviour.
	if (!g_Renderer.m_EnableAnimations)
		return;

	if (m_Animations.empty()) return;

	for (Animation& anim : m_Animations)
	{
		anim.m_CurrentTime += deltaTime;
		if (anim.m_Duration > 0)
			anim.m_CurrentTime = fmodf(anim.m_CurrentTime, anim.m_Duration);
	}

	m_InstanceDirtyRange = { UINT32_MAX, 0 };
	m_MaterialDirtyRange = { UINT32_MAX, 0 };

	for (const Animation& anim : m_Animations)
	{
		const float animTime = anim.m_CurrentTime;

		for (const AnimationChannel& channel : anim.m_Channels)
		{
			const AnimationSampler& sampler = anim.m_Samplers[channel.m_SamplerIndex];
			if (sampler.m_Inputs.empty()) continue;

			const Vector4 val = EvaluateAnimSampler(sampler, animTime);

			if (channel.m_Path == AnimationChannel::Path::EmissiveIntensity)
			{
				// Material emissive intensity: scale base emissive factor by animated scalar
				const float intensity = val.x;
				for (int mi = 0; mi < (int)channel.m_MaterialIndices.size(); ++mi)
				{
					const int matIdx = channel.m_MaterialIndices[mi];
					if (matIdx < 0 || matIdx >= (int)m_Materials.size()) continue;
					const Vector3& base = channel.m_BaseEmissiveFactor[mi];
					m_Materials[matIdx].m_EmissiveFactor = Vector3{
						base.x * intensity,
						base.y * intensity,
						base.z * intensity
					};
					// Track dirty range using the position of matIdx in m_DynamicMaterialIndices
					const auto it = std::lower_bound(m_DynamicMaterialIndices.begin(), m_DynamicMaterialIndices.end(), matIdx);
					if (it != m_DynamicMaterialIndices.end() && *it == matIdx)
					{
						const uint32_t slot = (uint32_t)std::distance(m_DynamicMaterialIndices.begin(), it);
						m_MaterialDirtyRange.first  = std::min(m_MaterialDirtyRange.first,  (uint32_t)matIdx);
						m_MaterialDirtyRange.second = std::max(m_MaterialDirtyRange.second, (uint32_t)matIdx);
						(void)slot;
					}
				}
			}
			else
			{
				using namespace DirectX;

				// Collect all node targets: primary (glTF) + multi-target (JSON)
				auto ApplyToNode = [&](int nodeIdx)
				{
					if (nodeIdx < 0 || nodeIdx >= (int)m_Nodes.size()) return;
					Node& node = m_Nodes[nodeIdx];
					node.m_IsDirty = true;

					if (channel.m_Path == AnimationChannel::Path::Translation)
					{
						XMStoreFloat3(&node.m_Translation, XMLoadFloat4(&val));
					}
					else if (channel.m_Path == AnimationChannel::Path::Rotation)
					{
						XMStoreFloat4(&node.m_Rotation, XMQuaternionNormalize(XMLoadFloat4(&val)));
					}
					else if (channel.m_Path == AnimationChannel::Path::Scale)
					{
						XMStoreFloat3(&node.m_Scale, XMLoadFloat4(&val));
					}
				};

				// All node targets (glTF single or JSON multi-target)
				for (int ni : channel.m_NodeIndices)
					ApplyToNode(ni);
			}
		}
	}

	// Update only dynamic nodes in topological order
	for (int idx : m_DynamicNodeIndices)
	{
		SDL_assert(idx >= 0 && idx < (int)m_Nodes.size() && "m_DynamicNodeIndices contains out-of-range index");
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

				// Update RT instance transform
				const Matrix& world = node.m_WorldTransform;
				nvrhi::rt::AffineTransform transform;
				transform[0] = world._11; transform[1] = world._21; transform[2] = world._31; transform[3] = world._41;
				transform[4] = world._12; transform[5] = world._22; transform[6] = world._32; transform[7] = world._42;
				transform[8] = world._13; transform[9] = world._23; transform[10] = world._33; transform[11] = world._43;
				m_RTInstanceDescs[instIdx].setTransform(transform);
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
	m_VertexBufferQuantized = nullptr;
	m_IndexBuffer = nullptr;
	m_MaterialConstantsBuffer = nullptr;
	m_InstanceDataBuffer = nullptr;
	m_MeshDataBuffer = nullptr;
	m_MeshletBuffer = nullptr;
	m_MeshletVerticesBuffer = nullptr;
	m_MeshletTrianglesBuffer = nullptr;
	m_LightBuffer = nullptr;
	m_TLAS = nullptr;
	m_RTInstanceDescBuffer = nullptr;
	m_BLASAddressBuffer = nullptr;
	m_InstanceLODBuffer = nullptr;
	m_RTInstanceDescs.clear();

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
	m_Animations.clear();
	m_DynamicMaterialIndices.clear();
	m_DynamicNodeIndices.clear();
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

void Scene::EnsureDefaultDirectionalLight()
{
    // Sort so directional lights come last (Spot < Point < Directional by enum value).
    std::sort(m_Lights.begin(), m_Lights.end(), [](const Scene::Light& a, const Scene::Light& b)
    {
        return a.m_Type > b.m_Type;
    });

    if (m_Lights.empty() || m_Lights.back().m_Type != Scene::Light::Directional)
    {
        Scene::Light light;
        light.m_Name      = "Default Directional";
        light.m_Type      = Scene::Light::Directional;
        light.m_Color     = Vector3{ 1.0f, 1.0f, 1.0f };
        light.m_Intensity = 1.0f;
        m_Lights.push_back(std::move(light));

        m_Lights.back().m_NodeIndex = (int)m_Nodes.size();
        Scene::Node& lightNode = m_Nodes.emplace_back();
        lightNode.m_LightIndex = (int)m_Lights.size() - 1;

        // Default sun angles: 45° pitch pointing downward along +Z in LH space.
        constexpr float defaultPitch = DirectX::XM_PIDIV4; // 45 degrees
        constexpr float defaultYaw   = 0.0f;
        DirectX::XMVECTOR quat = DirectX::XMQuaternionRotationRollPitchYaw(defaultPitch, defaultYaw, 0.0f);
        DirectX::XMStoreFloat4(&lightNode.m_Rotation, quat);

        const DirectX::XMMATRIX localM = DirectX::XMMatrixRotationQuaternion(DirectX::XMLoadFloat4(&lightNode.m_Rotation));
        DirectX::XMStoreFloat4x4(&lightNode.m_LocalTransform, localM);
        lightNode.m_WorldTransform = lightNode.m_LocalTransform;
    }
}
