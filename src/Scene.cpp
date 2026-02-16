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

	Renderer* renderer = Renderer::GetInstance();
	std::vector<VertexQuantized> allVerticesQuantized;
	std::vector<uint32_t> allIndices;

	SCOPED_TIMER("[Scene] LoadScene Total");

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
			success = SceneLoader::LoadGLTFScene(*this, scenePath, allVerticesQuantized, allIndices);
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

	SceneLoader::LoadTexturesFromImages(*this, sceneDir, renderer);
	SceneLoader::ApplyEnvironmentLights(*this);
	SceneLoader::UpdateMaterialsAndCreateConstants(*this, renderer);
	SceneLoader::CreateAndUploadGpuBuffers(*this, renderer, allVerticesQuantized, allIndices);
	SceneLoader::CreateAndUploadLightBuffer(*this, renderer);
	BuildAccelerationStructures();

	if (!m_Cameras.empty())
	{
		const Scene::Camera& firstCam = m_Cameras[0];
		renderer->SetCameraFromSceneCamera(firstCam);
		renderer->m_SelectedCameraIndex = 0;
	}
}

void Scene::BuildAccelerationStructures()
{
	SCOPED_TIMER("[Scene] Build Accel Structs");

    Renderer* renderer = Renderer::GetInstance();
    nvrhi::IDevice* device = renderer->m_RHI->m_NvrhiDevice;
	nvrhi::CommandListHandle cmd = renderer->AcquireCommandList();
	ScopedCommandList scopedCmd{ cmd, "Build Scene Accel Structs" };

    // Mapping from MeshDataIndex to Primitive pointer for TLAS build
    std::vector<Primitive*> meshDataToPrimitive(m_MeshData.size(), nullptr);

    // 1. Build BLAS for each primitive in each mesh
	for (Mesh& mesh : m_Meshes)
	{
		for (Primitive& primitive : mesh.m_Primitives)
		{
			SDL_assert(!primitive.m_BLAS);
            meshDataToPrimitive[primitive.m_MeshDataIndex] = &primitive;

			const MeshData& meshData = m_MeshData[primitive.m_MeshDataIndex];

			nvrhi::rt::GeometryDesc geometryDesc;
			nvrhi::rt::GeometryTriangles& geometryTriangle = geometryDesc.geometryData.triangles;
			geometryTriangle.indexBuffer = m_IndexBuffer;
			geometryTriangle.vertexBuffer = m_VertexBufferQuantized;
			geometryTriangle.indexFormat = nvrhi::Format::R32_UINT;
			geometryTriangle.vertexFormat = nvrhi::Format::RGB32_FLOAT;
			geometryTriangle.indexOffset = meshData.m_IndexOffsets[0] * nvrhi::getFormatInfo(geometryTriangle.indexFormat).bytesPerBlock;
			geometryTriangle.vertexOffset = 0; // Indices are already global relative to the start of the vertex buffer
			geometryTriangle.indexCount = meshData.m_IndexCounts[0];
			geometryTriangle.vertexCount = primitive.m_VertexCount;
			geometryTriangle.vertexStride = sizeof(VertexQuantized);

			geometryDesc.flags = nvrhi::rt::GeometryFlags::None; // can't be opaque since we have alpha tested materials that can be applied to this mesh
			geometryDesc.geometryType = nvrhi::rt::GeometryType::Triangles;

			nvrhi::rt::AccelStructDesc blasDesc;
			blasDesc.bottomLevelGeometries = { geometryDesc };
			blasDesc.debugName = "BLAS";
			blasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::AllowCompaction | nvrhi::rt::AccelStructBuildFlags::PreferFastTrace;

			primitive.m_BLAS = device->createAccelStruct(blasDesc);

			nvrhi::utils::BuildBottomLevelAccelStruct(scopedCmd, primitive.m_BLAS, blasDesc);
		}
	}

	// 2. Build TLAS for the scene
	nvrhi::rt::AccelStructDesc tlasDesc;
    tlasDesc.topLevelMaxInstances =  (uint32_t)m_InstanceData.size();
    tlasDesc.debugName = "Scene TLAS";
    tlasDesc.isTopLevel = true;
    m_TLAS = device->createAccelStruct(tlasDesc);

	SDL_assert(m_RTInstanceDescs.empty());
	for (uint32_t instanceID = 0; instanceID < m_InstanceData.size(); ++instanceID)
    {
		const PerInstanceData& instData = m_InstanceData[instanceID];
		Primitive* primitive = meshDataToPrimitive.at(instData.m_MeshDataIndex);
		const uint32_t alphaMode = m_Materials.at(primitive->m_MaterialIndex).m_AlphaMode;

        nvrhi::rt::InstanceDesc& instanceDesc = m_RTInstanceDescs.emplace_back();

		// Copy transform (transpose of row-vector matrix)
		const Matrix& world = instData.m_World;
		nvrhi::rt::AffineTransform transform;
		transform[0] = world._11; transform[1] = world._21; transform[2] = world._31; transform[3] = world._41;
		transform[4] = world._12; transform[5] = world._22; transform[6] = world._32; transform[7] = world._42;
		transform[8] = world._13; transform[9] = world._23; transform[10] = world._33; transform[11] = world._43;
		instanceDesc.setTransform(transform);

        nvrhi::rt::InstanceFlags instanceFlags = nvrhi::rt::InstanceFlags::None;
        instanceFlags = instanceFlags | ((alphaMode == ALPHA_MODE_OPAQUE) ? nvrhi::rt::InstanceFlags::ForceOpaque : nvrhi::rt::InstanceFlags::ForceNonOpaque);

        instanceDesc.instanceID = instanceID;
        instanceDesc.instanceMask = 1;
        instanceDesc.instanceContributionToHitGroupIndex = 0;
        instanceDesc.flags = instanceFlags;
        instanceDesc.blasDeviceAddress = primitive->m_BLAS->getDeviceAddress();
    }

    // Create RT instance desc buffer
    if (!m_RTInstanceDescs.empty())
    {
        nvrhi::BufferDesc rtInstDesc;
        rtInstDesc.byteSize = (uint32_t)(m_RTInstanceDescs.size() * sizeof(nvrhi::rt::InstanceDesc));
        rtInstDesc.debugName = "RTInstanceDescBuffer";
        rtInstDesc.isAccelStructBuildInput = true;
        rtInstDesc.initialState = nvrhi::ResourceStates::AccelStructBuildInput;
        rtInstDesc.keepInitialState = true;
        m_RTInstanceDescBuffer = device->createBuffer(rtInstDesc);

        // Initial upload
        scopedCmd->writeBuffer(m_RTInstanceDescBuffer, m_RTInstanceDescs.data(), rtInstDesc.byteSize);

        scopedCmd->buildTopLevelAccelStructFromBuffer(m_TLAS, m_RTInstanceDescBuffer, 0, (uint32_t)m_RTInstanceDescs.size());
    }
}

void Scene::FinalizeLoadedScene()
{
    SCOPED_TIMER("[Scene] Finalize Scene");

    // 1. Identify dynamic nodes and sort them topologically
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
    struct InstInfo { PerInstanceData data; int nodeIdx; };
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
            PerInstanceData inst{};
            inst.m_World = node.m_WorldTransform;
            inst.m_PrevWorld = node.m_WorldTransform;
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

    SDL_Log("[Scene] Finalized: Instances: Opaque: %u, Masked: %u, Transparent: %u", m_OpaqueBucket.m_Count, m_MaskedBucket.m_Count, m_TransparentBucket.m_Count);
}

void Scene::Update(float deltaTime)
{
	// Save current worlds as previous worlds for all instances
	for (PerInstanceData& inst : m_InstanceData)
	{
		inst.m_PrevWorld = inst.m_World;
	}

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
	m_RTInstanceDescs.clear();

	// Release environment texture handles
	m_RadianceTexture = nullptr;
	m_IrradianceTexture = nullptr;

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
