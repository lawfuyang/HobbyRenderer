#include "FeedbackTextureSet.h"
#include "FeedbackTexture.h"

namespace nvfeedback
{
    FeedbackTextureSet::~FeedbackTextureSet()
    {
        for (auto& texture : m_Textures)
        {
            if (texture)
                texture->RemoveFromTextureSet(this);
        }
        m_Textures.clear();
    }

    void FeedbackTextureSet::SetPrimaryTextureIndex(uint32_t index)
    {
        if (index >= m_Textures.size()) return;
        m_PrimaryTextureIndex = index;
        UpdateTextures();
    }

    FeedbackTexture* FeedbackTextureSet::GetTexture(uint32_t index)
    {
        if (index >= m_Textures.size()) return nullptr;
        return m_Textures[index];
    }

    void FeedbackTextureSet::UpdateTextures() const
    {
        for (auto& tex : m_Textures)
            tex->UpdateTextureSets();
    }

    bool FeedbackTextureSet::AddTexture(FeedbackTexture* texture)
    {
        if (!texture) return false;
        m_Textures.push_back(texture);
        texture->AddToTextureSet(this);
        UpdateTextures();
        return true;
    }

    bool FeedbackTextureSet::RemoveTexture(FeedbackTexture* texture)
    {
        if (!texture) return false;
        auto it = std::find(m_Textures.begin(), m_Textures.end(), texture);
        if (it == m_Textures.end()) return false;
        m_Textures.erase(it);
        texture->RemoveFromTextureSet(this);
        UpdateTextures();
        return true;
    }

} // namespace nvfeedback
