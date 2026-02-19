#include "Scene.h"

#include "meshoptimizer.h"

static constexpr uint32_t kSceneCacheMagic = 0x59464C52; // "RLFY"
static constexpr uint32_t kSceneCacheVersion = 21;

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

static void WriteMeshletDataCompressed(std::ostream& os, const std::vector<Meshlet>& meshlets, const std::vector<uint32_t>& meshletVertices, const std::vector<uint32_t>& meshletTriangles)
{
	std::vector<unsigned char> encoded(meshopt_encodeMeshletBound(kMaxMeshletVertices, kMaxMeshletTriangles));

	for (const Meshlet& m : meshlets)
	{
		const uint32_t* vertices = &meshletVertices[m.m_VertexOffset];

		unsigned char triangles[kMaxMeshletTriangles * 3];
		for (uint32_t i = 0; i < m.m_TriangleCount; ++i)
		{
			uint32_t packed = meshletTriangles[m.m_TriangleOffset + i];
			triangles[i * 3 + 0] = packed & 0xFF;
			triangles[i * 3 + 1] = (packed >> 8) & 0xFF;
			triangles[i * 3 + 2] = (packed >> 16) & 0xFF;
		}

		size_t encodedSize = meshopt_encodeMeshlet(encoded.data(), encoded.size(), vertices, m.m_VertexCount, triangles, m.m_TriangleCount);
		uint32_t size32 = (uint32_t)encodedSize;
		WritePOD(os, size32);
		os.write(reinterpret_cast<const char*>(encoded.data()), size32);
	}
}

static void ReadMeshletDataCompressed(std::istream& is, const std::vector<Meshlet>& meshlets, std::vector<uint32_t>& meshletVertices, std::vector<uint32_t>& meshletTriangles)
{
	std::vector<unsigned char> encoded;
	for (const Meshlet& m : meshlets)
	{
		uint32_t encodedSize;
		ReadPOD(is, encodedSize);

		encoded.resize(encodedSize);
		is.read(reinterpret_cast<char*>(encoded.data()), encodedSize);

		uint32_t* vertices = &meshletVertices[m.m_VertexOffset];
		unsigned char triangles[kMaxMeshletTriangles * 3];

		meshopt_decodeMeshlet(vertices, m.m_VertexCount, sizeof(uint32_t), triangles, m.m_TriangleCount, 3, encoded.data(), encodedSize);

		for (uint32_t i = 0; i < m.m_TriangleCount; ++i)
		{
			uint32_t i0 = triangles[i * 3 + 0];
			uint32_t i1 = triangles[i * 3 + 1];
			uint32_t i2 = triangles[i * 3 + 2];
			meshletTriangles[m.m_TriangleOffset + i] = i0 | (i1 << 8) | (i2 << 16);
		}
	}
}

void Scene::SaveToCache(const std::string& cachePath, const std::vector<uint32_t>& allIndices, const std::vector<VertexQuantized>& allVerticesQuantized)
{
	std::ofstream os(cachePath, std::ios::binary);
	if (!os.is_open())
	{
		SDL_LOG_ASSERT_FAIL("Failed to open cache for writing", "[Scene] Failed to open cache file for writing: %s", cachePath.c_str());
		return;
	}

	WritePOD(os, kSceneCacheMagic);
	WritePOD(os, kSceneCacheVersion);

	// Meshes
	WritePOD(os, m_Meshes.size());
	for (const Scene::Mesh& mesh : m_Meshes)
	{
		WritePOD(os, mesh.m_Primitives.size());
		for (const Scene::Primitive& prim : mesh.m_Primitives)
		{
			WritePOD(os, prim.m_VertexOffset);
			WritePOD(os, prim.m_VertexCount);
			WritePOD(os, prim.m_MaterialIndex);
			WritePOD(os, prim.m_MeshDataIndex);
		}
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
		WritePOD(os, mat.m_IOR);
		WritePOD(os, mat.m_TransmissionFactor);
		WritePOD(os, mat.m_ThicknessFactor);
		WritePOD(os, mat.m_AttenuationDistance);
		WritePOD(os, mat.m_AttenuationColor);
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
		WritePOD(os, cam.m_ExposureValue);
		WritePOD(os, cam.m_ExposureCompensation);
		WritePOD(os, cam.m_ExposureValueMin);
		WritePOD(os, cam.m_ExposureValueMax);
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
		WritePOD(os, light.m_AngularSize);
		WritePOD(os, light.m_NodeIndex);
	}

	WriteString(os, m_RadianceTexturePath);
	WriteString(os, m_IrradianceTexturePath);

	WriteVector(os, m_MeshData);
	WriteVector(os, m_Meshlets);
	WritePOD(os, m_MeshletVertices.size());
	WritePOD(os, m_MeshletTriangles.size());
	WriteMeshletDataCompressed(os, m_Meshlets, m_MeshletVertices, m_MeshletTriangles);
	WriteVector(os, allIndices);
	WriteVector(os, allVerticesQuantized);

	// animations
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
}

bool Scene::LoadFromCache(const std::string& cachePath, std::vector<uint32_t>& allIndices, std::vector<VertexQuantized>& allVerticesQuantized)
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

	// Meshes
	size_t meshCount;
	ReadPOD(is, meshCount);
	m_Meshes.resize(meshCount);
	for (Scene::Mesh& mesh : m_Meshes)
	{
		size_t primCount;
		ReadPOD(is, primCount);
		mesh.m_Primitives.resize(primCount);
		for (Scene::Primitive& prim : mesh.m_Primitives)
		{
			ReadPOD(is, prim.m_VertexOffset);
			ReadPOD(is, prim.m_VertexCount);
			ReadPOD(is, prim.m_MaterialIndex);
			ReadPOD(is, prim.m_MeshDataIndex);
			prim.m_BLAS = nullptr;
		}
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
		ReadPOD(is, mat.m_IOR);
		ReadPOD(is, mat.m_TransmissionFactor);
		ReadPOD(is, mat.m_ThicknessFactor);
		ReadPOD(is, mat.m_AttenuationDistance);
		ReadPOD(is, mat.m_AttenuationColor);
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
		ReadPOD(is, cam.m_ExposureValue);
		ReadPOD(is, cam.m_ExposureCompensation);
		ReadPOD(is, cam.m_ExposureValueMin);
		ReadPOD(is, cam.m_ExposureValueMax);
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
		ReadPOD(is, light.m_AngularSize);
		ReadPOD(is, light.m_NodeIndex);
	}

	ReadString(is, m_RadianceTexturePath);
	ReadString(is, m_IrradianceTexturePath);

	ReadVector(is, m_MeshData);
	ReadVector(is, m_Meshlets);
	size_t meshletVertexCount, meshletTriangleCount;
	ReadPOD(is, meshletVertexCount);
	ReadPOD(is, meshletTriangleCount);
	m_MeshletVertices.resize(meshletVertexCount);
	m_MeshletTriangles.resize(meshletTriangleCount);
	ReadMeshletDataCompressed(is, m_Meshlets, m_MeshletVertices, m_MeshletTriangles);
	ReadVector(is, allIndices);
	ReadVector(is, allVerticesQuantized);

	// animations
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