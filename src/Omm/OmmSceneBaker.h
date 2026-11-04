#pragma once

#include "OmmTypes.h"

#include "shaders/srrhi/cpp/Mesh.h"

class Scene;

// Bakes opacity micromaps for every alpha-masked primitive of the scene and stores them on
// Scene::Primitive::m_Omm (one entry per LOD). Only primitives whose base-colour texture is a streamed,
// block-compressed texture can be baked: the bake reads the alpha channel from the mapped DDS bytes, and there
// is no CPU copy of an uncompressed texture. Returns the number of primitives that ended up with at least one
// mask; callers can compare that against the number of masked primitives to see what was skipped.
size_t BakeSceneOmm(Scene& scene, const std::vector<srrhi::VertexQuantized>& vertices,
                    const std::vector<uint32_t>& indices, const OmmBakeParams& params);
