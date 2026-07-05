#pragma once

#include "FeedbackTexture.h"

// Forward declarations
class Scene;

namespace nvfeedback
{
    // ─── StreamingContext ────────────────────────────────────────────────────
    // Holds all per-scene streaming state: the FeedbackManager pointer,
    // the per-material texture sets, and the reverse-lookup map from
    // nvrhi::TextureHandle → FeedbackTexture*.
    //
    // BuildTextureSets() must be called after all FeedbackTextures have been
    // created (i.e. after SceneLoader completes) and after the FeedbackManager
    // is initialised.
    // ─────────────────────────────────────────────────────────────────────────

    struct StreamingContext
    {
        // Map from Scene::Material index → FeedbackTextureSet
        // Owned by StreamingContext — cleared via Clear() or destructor.
        std::unordered_map<int, std::unique_ptr<FeedbackTextureSet>> m_TextureSetsByMaterialIndex;

        // Reverse lookup: nvrhi::TextureHandle → FeedbackTexture*
        // Populated during SceneLoader::LoadTexture() for every streaming texture.
        std::unordered_map<nvrhi::ITexture*, FeedbackTexture*> m_FeedbackTexturesByHandle;

        // Build FeedbackTextureSet objects for all materials in the scene.
        // Must be called after all FeedbackTextures are registered.
        void BuildTextureSets(const Scene& scene);

        // Release all texture sets and clear the maps.
        void Clear();
    };

} // namespace nvfeedback
