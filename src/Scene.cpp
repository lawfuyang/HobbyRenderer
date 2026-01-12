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

static Vector4x4 XMMATRIXToFloat4x4(const DirectX::XMMATRIX& m)
{
	Vector4x4 out;
	DirectX::XMStoreFloat4x4(reinterpret_cast<DirectX::XMFLOAT4X4*>(&out), m);
	return out;
}

// Recursively compute world transforms for a Scene instance
static void ComputeWorldTransforms(Scene& scene, int nodeIndex, const DirectX::XMMATRIX& parent)
{
	Scene::Node& node = scene.m_Nodes[nodeIndex];
	DirectX::XMMATRIX local = DirectX::XMLoadFloat4x4(reinterpret_cast<const DirectX::XMFLOAT4X4*>(&node.m_LocalTransform));
	DirectX::XMMATRIX world = DirectX::XMMatrixMultiply(local, parent);
	node.m_WorldTransform = XMMATRIXToFloat4x4(world);

	for (int child : node.m_Children)
		ComputeWorldTransforms(scene, child, world);
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

	cgltf_options options{};
	cgltf_data* data = nullptr;
	cgltf_result res = cgltf_parse_file(&options, scenePath.c_str(), &data);
	if (res != cgltf_result_success || !data)
	{
		SDL_Log("[Scene] Failed to parse glTF file: %s", scenePath.c_str());
		return false;
	}

	res = cgltf_load_buffers(&options, data, scenePath.c_str());
	if (res != cgltf_result_success)
	{
		SDL_Log("[Scene] Failed to load glTF buffers");
		cgltf_free(data);
		SDL_assert(false && "glTF buffer load failed");
		return false;
	}

	// Clear instance containers
	m_Meshes.clear();
	m_Nodes.clear();
	m_Materials.clear();
	m_Textures.clear();

	// Textures & materials (minimal)
	for (cgltf_size i = 0; i < data->materials_count; ++i)
	{
		m_Materials.emplace_back();
		m_Materials.back().m_Name = data->materials[i].name ? data->materials[i].name : std::string();
	}

	for (cgltf_size i = 0; i < data->images_count; ++i)
	{
		m_Textures.emplace_back();
		m_Textures.back().m_Uri = data->images[i].uri ? data->images[i].uri : std::string();
	}

	// Collect vertex/index data
	std::vector<Vertex> allVertices;
	std::vector<uint32_t> allIndices;

	// Meshes/primitives
	for (cgltf_size mi = 0; mi < data->meshes_count; ++mi)
	{
		cgltf_mesh& cgMesh = data->meshes[mi];
		Mesh mesh;
		mesh.m_AabbMin = Vector3{ std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
		mesh.m_AabbMax = Vector3{ -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };

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
				SDL_Log("[Scene] Primitive missing POSITION attribute, skipping");
				continue;
			}

			const cgltf_size vertCount = posAcc->count;
			p.m_VertexOffset = static_cast<uint32_t>(allVertices.size());
			p.m_VertexCount = static_cast<uint32_t>(vertCount);

			// Read positions (and optionally normals/uvs)
			for (cgltf_size v = 0; v < vertCount; ++v)
			{
				Vertex vx{};
				float pos[3] = { 0,0,0 };
				cgltf_accessor_read_float(posAcc, v, pos, 3);
				vx.pos.x = pos[0]; vx.pos.y = pos[1]; vx.pos.z = pos[2];

				float nrm[3] = { 0,0,0 };
				if (normAcc)
					cgltf_accessor_read_float(normAcc, v, nrm, 3);
				vx.normal.x = nrm[0]; vx.normal.y = nrm[1]; vx.normal.z = nrm[2];

				float uv[2] = { 0,0 };
				if (uvAcc)
					cgltf_accessor_read_float(uvAcc, v, uv, 2);
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

	// Nodes: build simple list and hierarchy
	std::unordered_map<const cgltf_node*, int> nodeMap;
	for (cgltf_size ni = 0; ni < data->nodes_count; ++ni)
	{
		const cgltf_node& cn = data->nodes[ni];
		Node node;
		node.m_Name = cn.name ? cn.name : std::string();
		node.m_MeshIndex = cn.mesh ? static_cast<int>(cn.mesh - data->meshes) : -1;

		// local transform
		DirectX::XMMATRIX local = DirectX::XMMatrixIdentity();
		if (cn.has_matrix)
		{
			DirectX::XMFLOAT4X4 m;
			for (int i = 0; i < 16; ++i)
				reinterpret_cast<float*>(&m)[i] = cn.matrix[i];
			local = DirectX::XMLoadFloat4x4(&m);
		}
		else
		{
			DirectX::XMVECTOR trans = DirectX::XMVectorSet(0, 0, 0, 0);
			DirectX::XMVECTOR scale = DirectX::XMVectorSet(1, 1, 1, 0);
			DirectX::XMVECTOR rot = DirectX::XMQuaternionIdentity();
			if (cn.has_translation)
				trans = DirectX::XMVectorSet(cn.translation[0], cn.translation[1], cn.translation[2], 0);
			if (cn.has_scale)
				scale = DirectX::XMVectorSet(cn.scale[0], cn.scale[1], cn.scale[2], 0);
			if (cn.has_rotation)
				rot = DirectX::XMVectorSet(cn.rotation[0], cn.rotation[1], cn.rotation[2], cn.rotation[3]);

			local = DirectX::XMMatrixScalingFromVector(scale) * DirectX::XMMatrixRotationQuaternion(rot) * DirectX::XMMatrixTranslationFromVector(trans);
		}

		node.m_LocalTransform = XMMATRIXToFloat4x4(local);
		node.m_WorldTransform = node.m_LocalTransform;

		m_Nodes.push_back(std::move(node));
		nodeMap[&cn] = static_cast<int>(m_Nodes.size()) - 1;
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
				m_Nodes[idx].m_Children.push_back(childIdx);
				m_Nodes[childIdx].m_Parent = idx;
			}
		}
	}

	// Compute world transforms (roots are nodes with parent == -1)
	for (size_t i = 0; i < m_Nodes.size(); ++i)
	{
		if (m_Nodes[i].m_Parent == -1)
			ComputeWorldTransforms(*this, static_cast<int>(i), DirectX::XMMatrixIdentity());
	}

	// Compute per-node AABB by transforming mesh AABB into world space (simple approx: transform 8 corners)
	for (size_t ni = 0; ni < m_Nodes.size(); ++ni)
	{
		Node& node = m_Nodes[ni];
		if (node.m_MeshIndex >= 0 && node.m_MeshIndex < static_cast<int>(m_Meshes.size()))
		{
			Mesh& mesh = m_Meshes[node.m_MeshIndex];
			// initialize
			node.m_AabbMin = Vector3{ std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
			node.m_AabbMax = Vector3{ -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };

			DirectX::XMMATRIX world = DirectX::XMLoadFloat4x4(reinterpret_cast<const DirectX::XMFLOAT4X4*>(&node.m_WorldTransform));
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
				DirectX::XMVECTOR v = DirectX::XMLoadFloat3(reinterpret_cast<const DirectX::XMFLOAT3*>(&corners[c]));
				DirectX::XMVECTOR vt = DirectX::XMVector3Transform(v, world);
				DirectX::XMFLOAT3 vt3;
				DirectX::XMStoreFloat3(&vt3, vt);
				Vector3 tv{ vt3.x, vt3.y, vt3.z };
				ExtendAABB(node.m_AabbMin, node.m_AabbMax, tv);
			}
		}
	}

	// Create GPU buffers for all vertex/index data
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
	}

	if (ibytes > 0)
	{
		nvrhi::BufferDesc desc{};
		desc.byteSize = (uint32_t)ibytes;
		desc.isIndexBuffer = true;
		desc.initialState = nvrhi::ResourceStates::IndexBuffer;
		desc.keepInitialState = true;
		m_IndexBuffer = renderer->m_NvrhiDevice->createBuffer(desc);
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
	}

	cgltf_free(data);
	SDL_Log("[Scene] Loaded meshes: %zu, nodes: %zu", m_Meshes.size(), m_Nodes.size());
	return true;
}
