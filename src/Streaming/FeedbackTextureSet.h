#pragma once

#include "FeedbackTexture.h"

namespace nvfeedback
{
    class FeedbackTextureSet
    {
    public:
        ~FeedbackTextureSet();

        uint32_t GetNumTextures() const { return (uint32_t)m_Textures.size(); }

        void SetPrimaryTextureIndex(uint32_t index);
        uint32_t GetPrimaryTextureIndex() const { return m_PrimaryTextureIndex; }

        FeedbackTexture* GetTexture(uint32_t index);
        FeedbackTexture* GetPrimaryTexture() const
        {
            if (m_Textures.empty()) return nullptr;
            return m_Textures[m_PrimaryTextureIndex];
        }

        bool AddTexture(FeedbackTexture* texture);
        bool RemoveTexture(FeedbackTexture* texture);

    private:
        std::vector<FeedbackTexture*> m_Textures;
        uint32_t m_PrimaryTextureIndex = 0;

        void UpdateTextures() const;
        void UpdateTextureState();
    };

} // namespace nvfeedback
