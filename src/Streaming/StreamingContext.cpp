#include "StreamingContext.h"
#include "../Scene.h"
#include "../Renderer.h"

namespace nvfeedback
{
    void StreamingContext::BuildTextureSets(const Scene& scene)
    {
        // Release any existing sets
        m_TextureSetsByMaterialIndex.clear();

        uint32_t setsCreated = 0;

        for (int matIdx = 0; matIdx < (int)scene.m_Materials.size(); matIdx++)
        {
            const Scene::Material& mat = scene.m_Materials[matIdx];

            // ── Check: does this material have a primary (baseColor) texture? ──
            int baseColorIdx = mat.m_BaseColorTexture;
            if (baseColorIdx == -1) continue;

            const Scene::Texture& baseTex = scene.m_Textures[baseColorIdx];

            // ── Check: is this texture registered as a FeedbackTexture? ──
            auto it = m_FeedbackTexturesByHandle.find(baseTex.m_Handle.Get());
            if (it == m_FeedbackTexturesByHandle.end()) continue;

            FeedbackTexture* primaryFt = it->second;

            // ── Create the set ──
            auto texSet = g_Renderer.m_FeedbackManager->CreateTextureSet();

            // Helper: add texture if it's a registered FeedbackTexture
            auto tryAdd = [&](int texIdx) {
                if (texIdx == -1) return;
                if (texIdx >= (int)scene.m_Textures.size()) return;
                auto it2 = m_FeedbackTexturesByHandle.find(scene.m_Textures[texIdx].m_Handle.Get());
                if (it2 != m_FeedbackTexturesByHandle.end())
                    texSet->AddTexture(it2->second);
            };

            // Order matters: first texture added = primary
            tryAdd(mat.m_BaseColorTexture);           // PRIMARY
            tryAdd(mat.m_NormalTexture);              // follower
            tryAdd(mat.m_MetallicRoughnessTexture);   // follower
            tryAdd(mat.m_EmissiveTexture);            // follower

            // ── Validate: reject if follower exceeds primary ──
            uint32_t numTex = texSet->GetNumTextures();
            if (numTex <= 1)
            {
                continue;
            }

            nvrhi::TextureHandle primaryReserved = primaryFt->GetReservedTexture();
            uint32_t pw = primaryReserved->getDesc().width;
            uint32_t ph = primaryReserved->getDesc().height;
            uint32_t pm = primaryReserved->getDesc().mipLevels;

            bool bValid = true;
            for (uint32_t i = 1; i < numTex; i++)
            {
                FeedbackTexture* ft = texSet->GetTexture(i);
                if (!ft) continue;
                nvrhi::TextureHandle reserved = ft->GetReservedTexture();
                if (reserved->getDesc().width  > pw ||
                    reserved->getDesc().height > ph ||
                    reserved->getDesc().mipLevels > pm)
                {
                    bValid = false;
                    break;
                }
            }

            if (!bValid)
            {
                continue;
            }

            m_TextureSetsByMaterialIndex[matIdx] = std::move(texSet);
            setsCreated++;
        }

        SDL_Log("[Streaming] BuildTextureSets: created %u texture sets for %zu materials.",
                setsCreated, scene.m_Materials.size());
    }

    void StreamingContext::Clear()
    {
        m_TextureSetsByMaterialIndex.clear();
        m_FeedbackTexturesByHandle.clear();
    }

} // namespace nvfeedback
