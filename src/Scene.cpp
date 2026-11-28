#include "Scene.h"
#include "SceneLoader.h"
#include "SceneCache.h"
#include "ClusterLodGpu.h"
#include "Config.h"
#include "Renderer.h"
#include "PixCapture.h"
#include "CommonResources.h"
#include "Utilities.h"
#include "Omm/OmmSceneBaker.h"
#include "Omm/OmmRuntime.h"

// Capacities of the per-bucket traversal queues. These used to be sized by the
// instances x tree worst case, which is what a scene with one instance per node needs and what a scene with
// millions of EXT_mesh_gpu_instancing placements cannot afford: that worst case becomes billions of refs and
// tens of GB, while one frame can only use a few million. The shaders drop an append past the end and count it
// (ClusterCounters::m_RefCapped), so a cap costs image coverage, never a buffer overrun.
constexpr uint32_t kMaxClusterJobs      = 4u << 20;   // 33.5 MB per bucket at 8 B per job
constexpr uint32_t kMaxClusterNodeRefs  = 2u << 20;   // 25.2 MB per node queue at 12 B per ref
constexpr uint32_t kMaxClusterGroupRefs = 2u << 20;   // 25.2 MB per group queue at 12 B per ref

Scene::GeometryFootprint Scene::MeasureGeometryFootprint() const
{
	GeometryFootprint footprint;

	// A triangle BLAS references the scene's own vertex block and index range, so both have to be resident for as
	// long as the AS exists - that is the part of RT that disk-backed streaming cannot help with.
	for (const Mesh& mesh : m_Meshes)
	{
		for (const Primitive& primitive : mesh.m_Primitives)
			footprint.m_VertexBytes += uint64_t(primitive.m_VertexCount) * sizeof(srrhi::VertexQuantized);
	}

	uint64_t indexCount = 0;
	for (const srrhi::MeshData& meshData : m_MeshData)
	{
		const uint32_t lodCount = std::min<uint32_t>(meshData.m_LODCount, srrhi::CommonConsts::MAX_LOD_COUNT);
		for (uint32_t lod = 0; lod < lodCount; ++lod)
		{
			indexCount += meshData.m_IndexCounts[lod];
			footprint.m_Triangles += meshData.m_IndexCounts[lod] / 3u;
		}
	}
	footprint.m_IndexBytes = indexCount * sizeof(uint32_t);

	// Measured, not guessed: threedscans_animals' 18.66 M triangles build 1 291.69 MB of BLAS at
	// PreferFastTrace (Scene.cpp's own log line), i.e. 69 B per triangle.
	constexpr uint64_t kBlasBytesPerTriangle = 69;
	footprint.m_BlasBytes = footprint.m_Triangles * kBlasBytesPerTriangle;
	footprint.m_ResidentBytes = footprint.m_VertexBytes + footprint.m_IndexBytes + footprint.m_BlasBytes;
	return footprint;
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
    struct InstInfo { srrhi::PerInstanceData data; int nodeIdx; uint32_t transformIndex; };
    std::vector<InstInfo> opaqueStatic, opaqueDynamic;
    std::vector<InstInfo> maskedStatic, maskedDynamic;
    std::vector<InstInfo> transparentStatic, transparentDynamic;

    for (int ni = 0; ni < (int)m_Nodes.size(); ++ni)
    {
        Node& node = m_Nodes[ni];
        if (node.m_MeshIndex < 0) continue;
        const Mesh& mesh = m_Meshes[node.m_MeshIndex];

        // A node with EXT_mesh_gpu_instancing placements feeds one instance per placement: same mesh and same
        // materials, its own world matrix and its own bounding sphere (a placement can scale). Without placements
        // the node is the single instance, exactly as before. The node's own sphere is widened to cover the
        // placements, because the scene bounds built from the nodes' spheres must still contain the scene.
        const uint32_t placementCount = node.m_InstanceTransforms.empty() ? 1u : uint32_t(node.m_InstanceTransforms.size());
        DirectX::BoundingSphere placementBounds{};
        bool bHasPlacementBounds = false;

        for (uint32_t placement = 0; placement < placementCount; ++placement)
        {
            Matrix world;
            Vector3 center;
            float radius;
            if (node.m_InstanceTransforms.empty())
            {
                world = node.m_WorldTransform;
                center = node.m_Center;
                radius = node.m_Radius;
            }
            else
            {
                // Row-vector convention, as in Scene::Update: a placement lives in the node's own space.
                DirectX::XMStoreFloat4x4(&world, DirectX::XMMatrixMultiply(
                    DirectX::XMLoadFloat4x4(&node.m_InstanceTransforms[placement]),
                    DirectX::XMLoadFloat4x4(&node.m_WorldTransform)));

                const DirectX::BoundingSphere localSphere(mesh.m_Center, mesh.m_Radius);
                DirectX::BoundingSphere worldSphere;
                localSphere.Transform(worldSphere, DirectX::XMLoadFloat4x4(&world));
                center = worldSphere.Center;
                radius = worldSphere.Radius;

                if (bHasPlacementBounds)
                    DirectX::BoundingSphere::CreateMerged(placementBounds, placementBounds, worldSphere);
                else
                {
                    placementBounds = worldSphere;
                    bHasPlacementBounds = true;
                }
            }

            for (const Primitive& prim : mesh.m_Primitives)
            {
                srrhi::PerInstanceData inst{};
                inst.m_World = world;
                inst.m_PrevWorld = world;
                inst.m_MaterialIndex = prim.m_MaterialIndex;
                inst.m_MeshDataIndex = prim.m_MeshDataIndex;
                inst.m_Center = center;
                inst.m_Radius = radius;

                uint32_t alphaMode = prim.m_MaterialIndex >= 0 ? m_Materials[prim.m_MaterialIndex].m_GPU.m_AlphaMode : srrhi::CommonConsts::ALPHA_MODE_OPAQUE;
                bool isDynamic = node.m_IsDynamic;

                if (alphaMode == srrhi::CommonConsts::ALPHA_MODE_OPAQUE) {
                    if (isDynamic) opaqueDynamic.push_back({ inst, ni, placement });
                    else opaqueStatic.push_back({ inst, ni, placement });
                }
                else if (alphaMode == srrhi::CommonConsts::ALPHA_MODE_MASK) {
                    if (isDynamic) maskedDynamic.push_back({ inst, ni, placement });
                    else maskedStatic.push_back({ inst, ni, placement });
                }
                else {
                    if (isDynamic) transparentDynamic.push_back({ inst, ni, placement });
                    else transparentStatic.push_back({ inst, ni, placement });
                }
            }
        }

        if (bHasPlacementBounds)
        {
            node.m_Center = placementBounds.Center;
            node.m_Radius = placementBounds.Radius;
        }
    }

    m_OpaqueBucket = { 0, (uint32_t)(opaqueStatic.size() + opaqueDynamic.size()) };
    m_MaskedBucket = { m_OpaqueBucket.m_BaseIndex + m_OpaqueBucket.m_Count, (uint32_t)(maskedStatic.size() + maskedDynamic.size()) };
    m_TransparentBucket = { m_MaskedBucket.m_BaseIndex + m_MaskedBucket.m_Count, (uint32_t)(transparentStatic.size() + transparentDynamic.size()) };

    auto PushInstances = [&](const std::vector<InstInfo>& infos)
    {
        for (const InstInfo& info : infos)
        {
            m_Nodes[info.nodeIdx].m_InstanceIndices.push_back({ (uint32_t)m_InstanceData.size(), info.transformIndex });
            m_InstanceData.push_back(info.data);
        }
    };

    PushInstances(opaqueStatic);
    PushInstances(opaqueDynamic);
    PushInstances(maskedStatic);
    PushInstances(maskedDynamic);
    PushInstances(transparentStatic);
	PushInstances(transparentDynamic);

	// Worst-case cluster traversal work: every instance drawing every cluster, group and node of its
	// primitive. The primitive is found through its MeshDataIndex, which is what the traversal indexes too.
	std::unordered_map<uint32_t, const ClusterLod::PrimitiveClusters*> clustersByMeshData;
	for (const Mesh& mesh : m_Meshes)
	{
		for (size_t pi = 0; pi < mesh.m_Primitives.size() && pi < mesh.m_Clusters.size(); ++pi)
			clustersByMeshData[mesh.m_Primitives[pi].m_MeshDataIndex] = &mesh.m_Clusters[pi];
	}

	// The reject bitmask of the two-pass culling is indexed by a cluster's global record index, so its width is the
	// record count of the whole scene - every primitive's clusters once, however many instances draw them.
	m_ClusterRecordCount = 0;
	for (const auto& entry : clustersByMeshData)
		m_ClusterRecordCount += uint32_t(entry.second->m_Clusters.size());

	// Bounded per bucket rather than over the whole scene: each bucket owns its own traversal buffers, and the
	// masked bucket is usually a small fraction of the opaque one, so a scene-wide bound would reserve that
	// capacity a second time (see BasePassResources::DeclareResources for the trade-off this buys).
	const BucketInfo* const bucketInfos[kClusterBucketCount] = { &m_OpaqueBucket, &m_MaskedBucket };
	m_MaxClusterTreeDepth = 0;
	for (uint32_t bucket = 0; bucket < kClusterBucketCount; ++bucket)
	{
		// 64-bit: an instanced scene's worst case exceeds uint32, and it is clamped to a budget below anyway.
		uint64_t jobCount = 0;
		uint64_t groupCount = 0;
		uint64_t nodeCount = 0;

		for (uint32_t i = 0; i < bucketInfos[bucket]->m_Count; ++i)
		{
			const srrhi::PerInstanceData& inst = m_InstanceData[bucketInfos[bucket]->m_BaseIndex + i];
			const auto it = clustersByMeshData.find(inst.m_MeshDataIndex);
			if (it == clustersByMeshData.end())
				continue;

			const ClusterLod::Counts counts = ClusterLod::GetCounts(*it->second);
			jobCount += counts.m_ClusterCount;
			groupCount += counts.m_GroupCount;
			nodeCount += counts.m_NodeCount;
			m_MaxClusterTreeDepth = std::max(m_MaxClusterTreeDepth, counts.m_MaxTreeDepth);
		}

		// At least one: these size structured buffers, and a zero-sized one is invalid. The worst case is capped:
		// it is every instance visiting its whole tree, which for an instanced scene is billions of refs and tens
		// of GB, while one frame can only emit a fraction of that. The traversal drops an append past the end and
		// counts it (ClusterCounters::m_RefCapped), so a cap costs coverage, not correctness.
		m_MaxClusterJobCount[bucket] = uint32_t(std::clamp<uint64_t>(jobCount, 1ull, kMaxClusterJobs));
		m_MaxClusterGroupCount[bucket] = uint32_t(std::clamp<uint64_t>(groupCount, 1ull, kMaxClusterGroupRefs));
		m_MaxClusterNodeCount[bucket] = uint32_t(std::clamp<uint64_t>(nodeCount, 1ull, kMaxClusterNodeRefs));
	}

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

	const std::string filename = sceneFilePath.filename().string();
	const bool bIsSceneJson = filename.size() >= 11 && filename.substr(filename.size() - 11) == ".scene.json";

	bool success = false;
	if (bIsSceneJson)
	{
		success = SceneLoader::LoadJSONScene(*this, scenePath, allVerticesQuantized, allIndices);
	}
	else
	{
		// Load non-mesh data from glTF (materials, nodes, textures, cameras, lights, animations)
		success = SceneLoader::LoadGLTFScene_NonMesh(*this, scenePath, /*bFromJSONScene=*/false);
		if (success)
		{
			// Load or cook mesh data (cache-aware)
			success = SceneCache::LoadOrCookMeshData(sceneFilePath, *this, allVerticesQuantized, allIndices,
				g_Renderer.m_ClusterBlobFile);
				// A streamed (cluster-only) scene has no scene-wide geometry arrays; nothing that needs them can
				// run for it, RT included.
				m_bLegacyGeometryAbsent = success && allVerticesQuantized.empty() && allIndices.empty();
		}
	}

	if (!success)
	{
		SDL_LOG_ASSERT_FAIL("Scene load failed", "[Scene] Failed to load scene: %s", scenePath.c_str());
	}

	FinalizeLoadedScene();

	SceneLoader::LoadTexturesFromImages(*this, sceneDir);
	SceneLoader::UpdateMaterialsAndCreateConstants(*this);

	// A frame capture starts at a frame boundary, so the micromap and acceleration structure builds below are
	// never inside one. --pix-capture-load starts the bracket here instead, which is the capture to open when
	// the question is what the OMM arrays contain or which geometries link them.
	const bool bPixLoadCapture = Config::Get().m_bPixCaptureLoad && g_Pix.IsAvailable() &&
		g_Pix.BeginCaptureTagged("-load");

	if (g_Renderer.m_bOmmSupported && g_Renderer.m_bEnableOmm)
	{
		OmmBakeParams ommParams;
		// Command-line overrides exist so the shadow-edge A/B runs can vary the subdivision without a rebuild.
		if (Config::Get().m_OmmMaxLevel != 0)
			ommParams.m_MaxLevel = Config::Get().m_OmmMaxLevel;
		if (Config::Get().m_OmmTargetEdge > 0.0f)
			ommParams.m_TargetEdge = Config::Get().m_OmmTargetEdge;
		BakeSceneOmm(*this, allVerticesQuantized, allIndices, ommParams);
		BuildSceneOmm(*this);
	}
	SceneLoader::CreateAndUploadGpuBuffers(*this, allVerticesQuantized, allIndices);
	SceneLoader::CreateAndUploadLightBuffer(*this);
	BuildAccelerationStructures();

	// A frame capture starts at a frame boundary, so the micromap and acceleration structure builds above are
	// never inside one. --pix-capture-load brackets them here instead, which is the capture to open when the
	// question is what the OMM arrays contain or which geometries link them.
	if (bPixLoadCapture)
	{
		g_Renderer.ExecutePendingCommandLists();
		if (g_Pix.EndCapture())
			SDL_Log("[PIX] Load capture written: %s", g_Pix.GetNewestCapturePath().c_str());
		else
			SDL_Log("[PIX] Load capture was not written");
	}
}

uint64_t Scene::GetRayTracingBudgetBytes() const
{
	const uint32_t budgetMiB = Config::Get().m_RayTracingBudgetMiB;
	if (budgetMiB != 0)
		return uint64_t(budgetMiB) * 1024ull * 1024ull;
	return g_Renderer.GetStreamingBudgetBytes();
}

void Scene::BuildAccelerationStructures()
{
	//SCOPED_TIMER("[Scene] Build Accel Structs");

    nvrhi::IDevice* device = g_Renderer.m_RHI->m_NvrhiDevice;
	nvrhi::CommandListHandle cmd = g_Renderer.AcquireCommandList();
	ScopedCommandList scopedCmd{ cmd, "Build Scene Accel Structs" };

	// The OMM toggle rebuilds these, so the previous build is released rather than asserted against.
	for (Mesh& mesh : m_Meshes)
	{
		for (Primitive& primitive : mesh.m_Primitives)
			primitive.m_BLAS.clear();
	}
	m_RTInstanceDescs.clear();

	// A scene whose geometry does not fit in VRAM cannot have acceleration structures either: a triangle BLAS
	// is O(geometry) and, unlike the raster cut, it cannot be partially resident. Measured on
	// threedscans_animals: 69.2 B per triangle at PreferFastTrace (18.66 M triangles -> 1 291.69 MB), so at
	// 10^9 vertices the AS alone is tens of GB. Rather than let the driver fail the allocation, report the
	// footprint and run the scene with every ray-traced feature off (docs/cluster-lod-upstream-review-2026-11-23.md §20).
	const GeometryFootprint footprint = MeasureGeometryFootprint();
	const uint64_t budget = GetRayTracingBudgetBytes();
	// A streamed, cluster-only scene has no scene-wide vertex or index buffer at all, so there is nothing a
	// triangle BLAS could reference: RT is off for it whatever its size.
	const bool bHasGeometryBuffers = !m_bLegacyGeometryAbsent && m_VertexBufferQuantized != nullptr &&
		!m_MeshData.empty();
	if (!bHasGeometryBuffers)
	{
		SDL_Log("[RT ] Scene has no scene-wide geometry buffers (streamed import), so no BLAS/TLAS is built and "
		        "RT shadows and ReSTIR DI/GI are off.");
	}
	if (!bHasGeometryBuffers || footprint.m_ResidentBytes > budget)
	{
		if (bHasGeometryBuffers)
		{
			SDL_Log("[RT ] Scene geometry does not fit the ray-tracing budget: %.2f GB needed "
			        "(%.2f GB vertices + %.2f GB indices + %.2f GB BLAS for %llu triangles), %.2f GB available",
			        double(footprint.m_ResidentBytes) / 1e9, double(footprint.m_VertexBytes) / 1e9,
			        double(footprint.m_IndexBytes) / 1e9, double(footprint.m_BlasBytes) / 1e9,
			        (unsigned long long)footprint.m_Triangles, double(budget) / 1e9);
			SDL_Log("[RT ] Ray tracing disabled for this scene: no BLAS/TLAS is built, RT shadows and ReSTIR DI/GI "
			        "are off. Rasterisation is unaffected (clusters stream from the cooked file).");
		}

		m_bRayTracingDisabled = true;
		g_Renderer.m_EnableRTShadows = false;
		g_Renderer.m_EnableReSTIRDI = false;
		g_Renderer.m_IndirectLightingTechnique = srrhi::IndirectLightingMode::INDIRECT_LIGHTING_MODE_NONE;

		// An empty TLAS keeps every shader binding valid for the passes that are still recorded (the deferred
		// shader and the TLAS patch read the instance buffers even with the techniques off), at a cost of a few KB.
		// The build still needs a real instance buffer, so a one-element placeholder is uploaded and zero instances
		// are built from it.
		const uint32_t instanceDescBytes = uint32_t(sizeof(nvrhi::rt::InstanceDesc));
		nvrhi::BufferDesc instanceDescBuffer;
		instanceDescBuffer.byteSize = instanceDescBytes;
		instanceDescBuffer.structStride = instanceDescBytes;
		instanceDescBuffer.debugName = "RTInstanceDescBuffer (ray tracing disabled)";
		instanceDescBuffer.isAccelStructBuildInput = true;
		instanceDescBuffer.canHaveUAVs = true;
		instanceDescBuffer.initialState = nvrhi::ResourceStates::AccelStructBuildInput;
		instanceDescBuffer.keepInitialState = true;
		m_RTInstanceDescBuffer = device->createBuffer(instanceDescBuffer);

		nvrhi::rt::AccelStructDesc emptyTlasDesc{};
		emptyTlasDesc.topLevelMaxInstances = 1;
		emptyTlasDesc.debugName = "Scene TLAS (ray tracing disabled)";
		emptyTlasDesc.isTopLevel = true;
		m_TLAS = device->createAccelStruct(emptyTlasDesc);
		scopedCmd->buildTopLevelAccelStructFromBuffer(m_TLAS, m_RTInstanceDescBuffer, 0, 0);
		return;
	}
	m_bRayTracingDisabled = false;

    // Mapping from MeshDataIndex to Primitive pointer for TLAS build
    std::vector<Primitive*> meshDataToPrimitive(m_MeshData.size(), nullptr);

	// 1. Build one BLAS per LOD level per primitive
	uint64_t totalBLASMemoryBytes = 0;
	for (Mesh& mesh : m_Meshes)
	{
		for (Primitive& primitive : mesh.m_Primitives)
		{
			meshDataToPrimitive[primitive.m_MeshDataIndex] = &primitive;

			const srrhi::MeshData& meshData = m_MeshData[primitive.m_MeshDataIndex];
			const uint32_t lodCount = meshData.m_LODCount;
			SDL_assert(lodCount > 0 && lodCount <= srrhi::CommonConsts::MAX_LOD_COUNT);

			primitive.m_BLAS.resize(lodCount);

			for (uint32_t lod = 0; lod < lodCount; ++lod)
			{
				nvrhi::rt::GeometryDesc geometryDesc;
				nvrhi::rt::GeometryTriangles& geometryTriangle = geometryDesc.geometryData.triangles;
				// Ray tracing uses the locally-rebased index buffer, and declares the exact vertex
				// block those indices address. D3D12 requires every index to be less than
				// VertexCount; with the shared global vertex buffer and globally-rebased indices
				// that would have to be the whole scene's vertex count, which also makes tools
				// that snapshot the referenced vertex range (e.g. RenderDoc's replay) read
				// outside the data they captured.
				geometryTriangle.indexBuffer = m_RTIndexBuffer;
				geometryTriangle.vertexBuffer = m_VertexBufferQuantized;
				geometryTriangle.indexFormat = nvrhi::Format::R32_UINT;
				geometryTriangle.vertexFormat = nvrhi::Format::RGB32_FLOAT;
				geometryTriangle.indexOffset = meshData.m_IndexOffsets[lod] * nvrhi::getFormatInfo(geometryTriangle.indexFormat).bytesPerBlock;
				geometryTriangle.vertexOffset = (uint64_t)primitive.m_VertexOffset * sizeof(srrhi::VertexQuantized);
				geometryTriangle.indexCount = meshData.m_IndexCounts[lod];
				geometryTriangle.vertexCount = primitive.m_VertexCount;
				geometryTriangle.vertexStride = sizeof(srrhi::VertexQuantized);

				// The masks are linked per geometry, so a LOD baked for this primitive rides along with its BLAS.
				if (g_Renderer.m_bEnableOmm && lod < primitive.m_Omm.size() && primitive.m_Omm[lod].m_Array)
				{
					const OmmLodData& omm = primitive.m_Omm[lod];
					SDL_assert(omm.m_IndexCount == geometryTriangle.indexCount);

					geometryTriangle.setOpacityMicromap(omm.m_Array);
					geometryTriangle.setOmmIndexBuffer(omm.m_IndexBuffer);
					geometryTriangle.setOmmIndexBufferOffset(0);
					geometryTriangle.setOmmIndexFormat(omm.m_Stats.m_bSixteenBitIndices ? nvrhi::Format::R16_UINT
					                                                                : nvrhi::Format::R32_UINT);
					geometryTriangle.setPOmmUsageCounts(omm.m_Counts.data());
					geometryTriangle.setNumOmmUsageCounts(uint32_t(omm.m_Counts.size()));
				}

				geometryDesc.flags = nvrhi::rt::GeometryFlags::None; // can't be opaque since we have alpha tested materials that can be applied to this mesh
				geometryDesc.geometryType = nvrhi::rt::GeometryType::Triangles;

				nvrhi::rt::AccelStructDesc blasDesc;
				blasDesc.bottomLevelGeometries = { geometryDesc };
				blasDesc.debugName = (std::string("BLAS_LOD") + std::to_string(lod)).c_str();
				blasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastTrace;

				primitive.m_BLAS[lod] = device->createAccelStruct(blasDesc);
				nvrhi::utils::BuildBottomLevelAccelStruct(scopedCmd, primitive.m_BLAS[lod], blasDesc);

				// Accumulate memory for logging: the RT residency question is decided entirely by this number.
				totalBLASMemoryBytes += device->getAccelStructMemoryRequirements(primitive.m_BLAS[lod]).size;
			}
		}
	}

	SDL_Log("[Scene] Total BLAS memory across all LODs: %.2f MB", totalBLASMemoryBytes / (1024.0 * 1024.0));

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
		const uint32_t alphaMode = primitive->m_MaterialIndex != -1 ? m_Materials.at(primitive->m_MaterialIndex).m_GPU.m_AlphaMode : srrhi::CommonConsts::ALPHA_MODE_OPAQUE;

        nvrhi::rt::InstanceDesc& instanceDesc = m_RTInstanceDescs.emplace_back();

		// Copy transform (transpose of row-vector matrix)
		const Matrix& world = instData.m_World;
		nvrhi::rt::AffineTransform transform;
		transform[0] = world._11; transform[1] = world._21; transform[2] = world._31; transform[3] = world._41;
		transform[4] = world._12; transform[5] = world._22; transform[6] = world._32; transform[7] = world._42;
		transform[8] = world._13; transform[9] = world._23; transform[10] = world._33; transform[11] = world._43;
		instanceDesc.setTransform(transform);

        nvrhi::rt::InstanceFlags instanceFlags = nvrhi::rt::InstanceFlags::None;
        // A linked opacity micromap replaces the geometry-wide opacity state, and the force flags are applied
        // *after* the micromap classification: FORCE_NON_OPAQUE would turn every opaque micro-triangle back into
        // a candidate and send it through the shader's alpha test, which is the work the micromap exists to
        // remove. An instance whose BLAS carries masks therefore leaves both flags clear.
        const bool bHasOmm = g_Renderer.m_bEnableOmm && !primitive->m_Omm.empty() && primitive->m_Omm[0].m_Array != nullptr;
        if (alphaMode == srrhi::CommonConsts::ALPHA_MODE_OPAQUE)
            instanceFlags = instanceFlags | nvrhi::rt::InstanceFlags::ForceOpaque;
        else if (!bHasOmm)
            instanceFlags = instanceFlags | nvrhi::rt::InstanceFlags::ForceNonOpaque;

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
					m_Materials[matIdx].m_GPU.m_EmissiveFactor = Vector4{
						base.x * intensity,
						base.y * intensity,
						base.z * intensity,
						1.0f
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
			for (const Node::InstanceRef& ref : node.m_InstanceIndices)
			{
				const uint32_t instIdx = ref.m_InstanceIndex;

				if (node.m_InstanceTransforms.empty())
				{
					m_InstanceData[instIdx].m_World = node.m_WorldTransform;
					m_InstanceData[instIdx].m_Center = node.m_Center;
					m_InstanceData[instIdx].m_Radius = node.m_Radius;
				}
				else
				{
					// A placement follows the node: node world times the placement, and its own sphere.
					const DirectX::XMMATRIX worldM = DirectX::XMMatrixMultiply(
						DirectX::XMLoadFloat4x4(&node.m_InstanceTransforms[ref.m_TransformIndex]),
						DirectX::XMLoadFloat4x4(&node.m_WorldTransform));
					DirectX::XMStoreFloat4x4(&m_InstanceData[instIdx].m_World, worldM);

					const Mesh& mesh = m_Meshes[node.m_MeshIndex];
					const DirectX::BoundingSphere localSphere(mesh.m_Center, mesh.m_Radius);
					DirectX::BoundingSphere worldSphere;
					localSphere.Transform(worldSphere, worldM);
					m_InstanceData[instIdx].m_Center = worldSphere.Center;
					m_InstanceData[instIdx].m_Radius = worldSphere.Radius;
				}

				m_InstanceDirtyRange.first = std::min(m_InstanceDirtyRange.first, instIdx);
				m_InstanceDirtyRange.second = std::max(m_InstanceDirtyRange.second, instIdx);

				// Update RT instance transform
				if (instIdx < m_RTInstanceDescs.size())
				{
					const Matrix& world = m_InstanceData[instIdx].m_World;
					nvrhi::rt::AffineTransform transform;
					transform[0] = world._11; transform[1] = world._21; transform[2] = world._31; transform[3] = world._41;
					transform[4] = world._12; transform[5] = world._22; transform[6] = world._32; transform[7] = world._42;
					transform[8] = world._13; transform[9] = world._23; transform[10] = world._33; transform[11] = world._43;
					m_RTInstanceDescs[instIdx].setTransform(transform);
				}
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
	// Save current camera state before tearing down the scene
	if (!Config::Get().m_ScenePath.empty())
	{
		CameraSavedState state;
		state.position = m_Camera.GetPosition();
		state.yaw     = m_Camera.GetYaw();
		state.pitch   = m_Camera.GetPitch();
		g_Renderer.m_CameraStateManager.SaveCamera(Config::Get().m_ScenePath, state);
	}

	// Reset to a freshly constructed state: the implicit copy assignment releases the old GPU
	// handles and containers on the way through, so no per-member release list has to be kept in
	// sync with the class definition.
	*this = Scene{};
}
