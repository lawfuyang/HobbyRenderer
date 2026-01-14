#include "Scene.h"
#include "Config.h"
#include "Renderer.h"
#include "CommonResources.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

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

bool Scene::LoadScene()
{
	const std::string& scenePath = Config::Get().m_GltfScene;
	if (scenePath.empty())
	{
		SDL_Log("[Scene] No glTF scene configured, skipping load");
		return true;
	}

	SDL_Log("[Scene] Loading glTF scene: %s", scenePath.c_str());

	uint64_t t_start = SDL_GetTicks();

	cgltf_options options{};
	cgltf_data* data = nullptr;
	cgltf_result res = cgltf_parse_file(&options, scenePath.c_str(), &data);
	uint64_t t_parse = SDL_GetTicks();
	SDL_Log("[Scene] Parsed glTF in %llu ms", (unsigned long long)(t_parse - t_start));
	if (res != cgltf_result_success || !data)
	{
		SDL_LOG_ASSERT_FAIL("glTF parse failed", "[Scene] Failed to parse glTF file: %s", scenePath.c_str());
		return false;
	}

	res = cgltf_load_buffers(&options, data, scenePath.c_str());
	uint64_t t_loadBuf = SDL_GetTicks();
	SDL_Log("[Scene] Loaded buffers in %llu ms", (unsigned long long)(t_loadBuf - t_parse));
	if (res != cgltf_result_success)
	{
		SDL_LOG_ASSERT_FAIL("glTF buffer load failed", "[Scene] Failed to load glTF buffers");
		cgltf_free(data);
		return false;
	}

	// Textures & materials (minimal)
	uint64_t t_texmat_start = SDL_GetTicks();
	uint64_t t_texmat_end = t_texmat_start;
	for (cgltf_size i = 0; i < data->materials_count; ++i)
	{
		m_Materials.emplace_back();
		m_Materials.back().m_Name = data->materials[i].name ? data->materials[i].name : std::string();
		// Read PBR baseColorFactor if present
		const cgltf_pbr_metallic_roughness& pbr = data->materials[i].pbr_metallic_roughness;
		// copy base color factor (defaults to 1,1,1,1 if not specified)
		m_Materials.back().m_BaseColorFactor.x = pbr.base_color_factor[0];
		m_Materials.back().m_BaseColorFactor.y = pbr.base_color_factor[1];
		m_Materials.back().m_BaseColorFactor.z = pbr.base_color_factor[2];
		m_Materials.back().m_BaseColorFactor.w = pbr.base_color_factor[3];
		if (pbr.base_color_texture.texture)
		{
			const cgltf_texture* tex = pbr.base_color_texture.texture;
			if (tex && tex->image)
			{
				cgltf_size imgIndex = tex->image - data->images;
				m_Materials.back().m_BaseColorTexture = static_cast<int>(imgIndex);
			}
		}
		// metallic / roughness
		m_Materials.back().m_RoughnessFactor = pbr.roughness_factor;
		// If the glTF omits metallic information, cgltf gives the default 1.0.
		// Treat a missing metallicFactor and missing metallic_roughness_texture as non-metallic (0.0)
		float metallic = pbr.metallic_factor;
		if (pbr.metallic_roughness_texture.texture == NULL && metallic == 1.0f)
			metallic = 0.0f;
		m_Materials.back().m_MetallicFactor = metallic;
	}

	for (cgltf_size i = 0; i < data->images_count; ++i)
	{
		m_Textures.emplace_back();
		m_Textures.back().m_Uri = data->images[i].uri ? data->images[i].uri : std::string();
	}
	t_texmat_end = SDL_GetTicks();
	SDL_Log("[Scene] Materials+Textures in %llu ms", (unsigned long long)(t_texmat_end - t_texmat_start));

	// Cameras
	uint64_t t_cameras_start = SDL_GetTicks();
	for (cgltf_size i = 0; i < data->cameras_count; ++i)
	{
		const cgltf_camera& cgCam = data->cameras[i];
		Camera cam;
		cam.m_Name = cgCam.name ? cgCam.name : std::string();
		if (cgCam.type == cgltf_camera_type_perspective)
		{
			const auto& p = cgCam.data.perspective;
			cam.m_Projection.aspectRatio = p.has_aspect_ratio ? p.aspect_ratio : (16.0f / 9.0f);
			cam.m_Projection.fovY = p.yfov;
			cam.m_Projection.nearZ = p.znear;
			// farZ is always infinite, ignore p.zfar
			m_Cameras.push_back(std::move(cam));
		}
		else if (cgCam.type == cgltf_camera_type_orthographic)
		{
			// Skip orthographic cameras
			SDL_Log("[Scene] Skipping orthographic camera: %s", cam.m_Name.c_str());
		}
		else
		{
			SDL_Log("[Scene] Unknown camera type for camera: %s", cam.m_Name.c_str());
		}
	}
	uint64_t t_cameras_end = SDL_GetTicks();
	SDL_Log("[Scene] Cameras in %llu ms", (unsigned long long)(t_cameras_end - t_cameras_start));

	// Lights
	uint64_t t_lights_start = SDL_GetTicks();
	for (cgltf_size i = 0; i < data->lights_count; ++i)
	{
		const cgltf_light& cgLight = data->lights[i];
		Light light;
		light.m_Name = cgLight.name ? cgLight.name : std::string();
		light.m_Color = Vector3{ cgLight.color[0], cgLight.color[1], cgLight.color[2] };
		light.m_Intensity = cgLight.intensity;
		light.m_Range = cgLight.range;
		light.m_SpotInnerConeAngle = cgLight.spot_inner_cone_angle;
		light.m_SpotOuterConeAngle = cgLight.spot_outer_cone_angle;
		if (cgLight.type == cgltf_light_type_directional)
			light.m_Type = Light::Directional;
		else if (cgLight.type == cgltf_light_type_point)
			light.m_Type = Light::Point;
		else if (cgLight.type == cgltf_light_type_spot)
			light.m_Type = Light::Spot;
		m_Lights.push_back(std::move(light));
	}
	uint64_t t_lights_end = SDL_GetTicks();
	SDL_Log("[Scene] Lights in %llu ms", (unsigned long long)(t_lights_end - t_lights_start));

	// Collect vertex/index data
	uint64_t t_mesh_start = SDL_GetTicks();
	std::vector<Vertex> allVertices;
	std::vector<uint32_t> allIndices;

	// Meshes/primitives
	for (cgltf_size mi = 0; mi < data->meshes_count; ++mi)
	{
		cgltf_mesh& cgMesh = data->meshes[mi];
		Mesh mesh;
		mesh.m_AabbMin = Vector3{ FLT_MAX, FLT_MAX, FLT_MAX };
		mesh.m_AabbMax = Vector3{ -FLT_MAX, -FLT_MAX, -FLT_MAX };

		for (cgltf_size pi = 0; pi < cgMesh.primitives_count; ++pi)
		{
			cgltf_primitive& prim = cgMesh.primitives[pi];
			Primitive p;

			// Find accessors
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
				//SDL_Log("[Scene] Primitive missing POSITION attribute, skipping");
				SDL_LOG_ASSERT_FAIL("Primitive missing POSITION attribute", "[Scene] Primitive missing POSITION attribute. Is this normal?");
				continue;
			}

			const cgltf_size vertCount = posAcc->count;
			p.m_VertexOffset = static_cast<uint32_t>(allVertices.size());
			p.m_VertexCount = static_cast<uint32_t>(vertCount);

			// Read positions (and optionally normals/uvs)
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
				allVertices.push_back(vx);
			}

			// Indices
			p.m_IndexOffset = static_cast<uint32_t>(allIndices.size());
			p.m_IndexCount = 0;
			if (prim.indices)
			{
				const cgltf_size idxCount = prim.indices->count;
				p.m_IndexCount = static_cast<uint32_t>(idxCount);
				// Read indices (cgltf_accessor_read_index returns cgltf_size)
				for (cgltf_size k = 0; k < idxCount; ++k)
				{
					cgltf_size rawIdx = cgltf_accessor_read_index(prim.indices, k);
					uint32_t idx = static_cast<uint32_t>(rawIdx);
					allIndices.push_back(static_cast<uint32_t>(p.m_VertexOffset) + idx);
				}
			}

			p.m_MaterialIndex = prim.material ? static_cast<int>(prim.material - data->materials) : -1;
			mesh.m_Primitives.push_back(p);
		}

		m_Meshes.push_back(std::move(mesh));
	}
	uint64_t t_mesh_end = SDL_GetTicks();
	SDL_Log("[Scene] Mesh processing (vtx+idx) in %llu ms", (unsigned long long)(t_mesh_end - t_mesh_start));

	// Nodes: build simple list and hierarchy
	uint64_t t_nodes_start = SDL_GetTicks();
	std::unordered_map<const cgltf_node*, int> nodeMap;
	for (cgltf_size ni = 0; ni < data->nodes_count; ++ni)
	{
		const cgltf_node& cn = data->nodes[ni];
		Node node;
		node.m_Name = cn.name ? cn.name : std::string();
		node.m_MeshIndex = cn.mesh ? static_cast<int>(cn.mesh - data->meshes) : -1;
		node.m_CameraIndex = cn.camera ? static_cast<int>(cn.camera - data->cameras) : -1;
		node.m_LightIndex = cn.light ? static_cast<int>(cn.light - data->lights) : -1;

		// local transform
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

		m_Nodes.push_back(std::move(node));
		nodeMap[&cn] = static_cast<int>(m_Nodes.size()) - 1;
	}
	uint64_t t_nodes_end = SDL_GetTicks();
	SDL_Log("[Scene] Node parsing in %llu ms", (unsigned long long)(t_nodes_end - t_nodes_start));

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
				m_Nodes[idx].m_Children.push_back(childIdx);
				m_Nodes[childIdx].m_Parent = idx;
			}
		}
	}

	// Set node indices in cameras and lights
	for (size_t i = 0; i < m_Nodes.size(); ++i)
	{
		const Node& node = m_Nodes[i];
		if (node.m_CameraIndex >= 0 && node.m_CameraIndex < static_cast<int>(m_Cameras.size()))
		{
			m_Cameras[node.m_CameraIndex].m_NodeIndex = static_cast<int>(i);
		}
		if (node.m_LightIndex >= 0 && node.m_LightIndex < static_cast<int>(m_Lights.size()))
		{
			m_Lights[node.m_LightIndex].m_NodeIndex = static_cast<int>(i);
		}
	}

	// Compute world transforms (roots are nodes with parent == -1)
	uint64_t t_xform_start = SDL_GetTicks();
	for (size_t i = 0; i < m_Nodes.size(); ++i)
	{
		if (m_Nodes[i].m_Parent == -1)
		{
			Matrix identity{};
			DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
			ComputeWorldTransforms(*this, static_cast<int>(i), identity);
		}
	}

	uint64_t t_xform_end = SDL_GetTicks();
	SDL_Log("[Scene] World transform computation in %llu ms", (unsigned long long)(t_xform_end - t_xform_start));
	// Compute per-node AABB by transforming mesh AABB into world space (simple approx: transform 8 corners)
	uint64_t t_aabb_start = SDL_GetTicks();
	for (size_t ni = 0; ni < m_Nodes.size(); ++ni)
	{
		Node& node = m_Nodes[ni];
		if (node.m_MeshIndex >= 0 && node.m_MeshIndex < static_cast<int>(m_Meshes.size()))
		{
			Mesh& mesh = m_Meshes[node.m_MeshIndex];
			// initialize
			node.m_AabbMin = Vector3{ FLT_MAX, FLT_MAX, FLT_MAX };
			node.m_AabbMax = Vector3{ -FLT_MAX, -FLT_MAX, -FLT_MAX };

			DirectX::XMMATRIX world = DirectX::XMLoadFloat4x4(&node.m_WorldTransform);
			// 8 corners
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
	uint64_t t_aabb_end = SDL_GetTicks();
	SDL_Log("[Scene] AABB computation in %llu ms", (unsigned long long)(t_aabb_end - t_aabb_start));

	// Set directional light from first GLTF directional light
	for (const auto& light : m_Lights)
	{
		if (light.m_Type == Light::Directional && light.m_NodeIndex >= 0 && light.m_NodeIndex < static_cast<int>(m_Nodes.size()))
		{
			const Matrix& worldTransform = m_Nodes[light.m_NodeIndex].m_WorldTransform;
			DirectX::XMMATRIX m = DirectX::XMLoadFloat4x4(&worldTransform);
			// For directional light, direction is the forward (-Z) of the node
			DirectX::XMVECTOR localDir = DirectX::XMVectorSet(0, 0, -1, 0);
			DirectX::XMVECTOR worldDir = DirectX::XMVector3TransformNormal(localDir, m);
			DirectX::XMFLOAT3 dir;
			DirectX::XMStoreFloat3(&dir, DirectX::XMVector3Normalize(worldDir));
			// Compute yaw and pitch from direction
			float yaw = atan2f(dir.x, dir.z);
			float pitch = asinf(dir.y);
			Renderer* renderer = Renderer::GetInstance();
			renderer->m_DirectionalLight.yaw = yaw;
			renderer->m_DirectionalLight.pitch = pitch;
			renderer->m_DirectionalLight.intensity = light.m_Intensity * 10000.0f; // assuming lux to our units
			break; // only first one
		}
	}

	// Set the first GLTF camera as the default camera
	if (!m_Cameras.empty())
	{
		const Camera& firstCam = m_Cameras[0];
		Renderer* renderer = Renderer::GetInstance();
		renderer->SetCameraFromSceneCamera(firstCam);
		renderer->m_SelectedCameraIndex = 0; // Set to first camera in dropdown
	}

	// Create GPU buffers for all vertex/index data
	uint64_t t_gpu_start = SDL_GetTicks();
	uint64_t t_gpu_end = t_gpu_start;
	Renderer* renderer = Renderer::GetInstance();

	const size_t vbytes = allVertices.size() * sizeof(Vertex);
	const size_t ibytes = allIndices.size() * sizeof(uint32_t);

	if (vbytes > 0)
	{
		nvrhi::BufferDesc desc{};
		desc.byteSize = (uint32_t)vbytes;
		desc.isVertexBuffer = true;
		desc.initialState = nvrhi::ResourceStates::VertexBuffer;
		desc.keepInitialState = true;
		m_VertexBuffer = renderer->m_NvrhiDevice->createBuffer(desc);
		renderer->m_RHI.SetDebugName(m_VertexBuffer, "Scene_VertexBuffer");
	}

	if (ibytes > 0)
	{
		nvrhi::BufferDesc desc{};
		desc.byteSize = (uint32_t)ibytes;
		desc.isIndexBuffer = true;
		desc.initialState = nvrhi::ResourceStates::IndexBuffer;
		desc.keepInitialState = true;
		m_IndexBuffer = renderer->m_NvrhiDevice->createBuffer(desc);
		renderer->m_RHI.SetDebugName(m_IndexBuffer, "Scene_IndexBuffer");
	}

	// Upload data using a command list
	if (m_VertexBuffer || m_IndexBuffer)
	{
		nvrhi::CommandListHandle cmd = renderer->AcquireCommandList("Upload Scene");
		if (m_VertexBuffer && vbytes > 0)
			cmd->writeBuffer(m_VertexBuffer, allVertices.data(), vbytes, 0);
		if (m_IndexBuffer && ibytes > 0)
			cmd->writeBuffer(m_IndexBuffer, allIndices.data(), ibytes, 0);

		renderer->SubmitCommandList(cmd);
		renderer->ExecutePendingCommandLists();
		t_gpu_end = SDL_GetTicks();
		SDL_Log("[Scene] GPU upload (create+write+submit) in %llu ms", (unsigned long long)(t_gpu_end - t_gpu_start));
	}

	cgltf_free(data);
	uint64_t t_end = SDL_GetTicks();
	SDL_Log("[Scene] Loaded meshes: %zu, nodes: %zu (total %llu ms)", m_Meshes.size(), m_Nodes.size(), (unsigned long long)(t_end - t_start));
	SDL_Log("[Scene] Breakdown ms: parse=%llu loadbuf=%llu texmat=%llu mesh=%llu nodes=%llu xform=%llu aabb=%llu gpu=%llu", 
		(unsigned long long)(t_parse - t_start),
		(unsigned long long)(t_loadBuf - t_parse),
		(unsigned long long)(t_texmat_end - t_texmat_start),
		(unsigned long long)(t_mesh_end - t_mesh_start),
		(unsigned long long)(t_nodes_end - t_nodes_start),
		(unsigned long long)(t_xform_end - t_xform_start),
		(unsigned long long)(t_aabb_end - t_aabb_start),
		(unsigned long long)(t_gpu_end - t_gpu_start));
	
	return true;
}

void Scene::Shutdown()
{
	// Release GPU buffer handles so NVRHI can free underlying resources
	m_VertexBuffer = nullptr;
	m_IndexBuffer = nullptr;

	// Clear CPU-side containers
	m_Meshes.clear();
	m_Nodes.clear();
	m_Materials.clear();
	m_Textures.clear();
	m_Cameras.clear();
	m_Lights.clear();
}
