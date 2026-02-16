#include "RenderGraph.h"
#include "Renderer.h"
#include "imgui.h"

using namespace RenderGraphInternal;

void RenderGraph::RenderDebugUI()
{
    nvrhi::IDevice* device = Renderer::GetInstance()->m_RHI->m_NvrhiDevice.Get();

    static ImGuiTextFilter visFilter;
    static ImGuiTextFilter texFilter;
    static ImGuiTextFilter bufFilter;
    static ImGuiTextFilter passFilter;
    static ImGuiTextFilter heapFilter;

    if (ImGui::TreeNode("Render Graph"))
    {
        ImGui::Text("Textures: %u (Allocated: %u, Aliased: %u)", 
                   m_Stats.m_NumTextures, 
                   m_Stats.m_NumAllocatedTextures, 
                   m_Stats.m_NumAliasedTextures);
        
        ImGui::Text("Buffers: %u (Allocated: %u, Aliased: %u)", 
                   m_Stats.m_NumBuffers, 
                   m_Stats.m_NumAllocatedBuffers, 
                   m_Stats.m_NumAliasedBuffers);
        
        ImGui::Text("Texture Memory: %.2f MB", 
                   m_Stats.m_TotalTextureMemory / (1024.0 * 1024.0));
        
        ImGui::Text("Buffer Memory: %.2f MB", 
                   m_Stats.m_TotalBufferMemory / (1024.0 * 1024.0));
        
        if (ImGui::TreeNode("Lifetime Visualization"))
        {
            visFilter.Draw("Filter Resources");

            const float kPassWidth = 25.0f;
            const float kRowHeight = 22.0f;
            const float kNameWidth = 200.0f;
            
            uint32_t numPasses = (uint32_t)m_PassNames.size();
            
            if (numPasses > 0)
            {
                ImGui::BeginChild("LifetimeScroll", ImVec2(0, 400), ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize, ImGuiWindowFlags_HorizontalScrollbar);
                
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                ImVec2 startPos = ImGui::GetCursorScreenPos();
                
                // Header - Pass names (vertical or angled if many)
                float headerHeight = 100.0f;
                // Adjust width based on filtered passes? No, passes stay the same, but rows are filtered.
                ImGui::Dummy(ImVec2(kNameWidth + numPasses * kPassWidth, headerHeight));
                
                for (uint32_t i = 0; i < numPasses; ++i)
                {
                    ImVec2 textPos = ImVec2(startPos.x + kNameWidth + i * kPassWidth + kPassWidth * 0.5f, startPos.y + headerHeight - 5.0f);
                    // Draw pass number and name rotated or vertically
                    char passLabel[16];
                    sprintf(passLabel, "%u", i + 1);
                    drawList->AddText(ImVec2(textPos.x - 5, startPos.y), ImGui::GetColorU32(ImGuiCol_Text), passLabel);
                    
                    // Simple vertical line
                    drawList->AddLine(ImVec2(startPos.x + kNameWidth + i * kPassWidth, startPos.y + 20), 
                                      ImVec2(startPos.x + kNameWidth + i * kPassWidth, startPos.y + 1000), 
                                      ImGui::GetColorU32(ImGuiCol_Separator, 0.5f));
                }

                auto drawResourceRow = [&](const auto& resource, const char* typeName, ImU32 color, const auto& resourceVector)
                {
                    if (!resource.m_IsDeclaredThisFrame || !resource.m_Lifetime.IsValid())
                        return;

                    const std::string& name = resource.m_Desc.m_NvrhiDesc.debugName;
                    if (!visFilter.PassFilter(name.c_str()))
                        return;

                    ImVec2 rowStart = ImGui::GetCursorScreenPos();
                    
                    ImGui::Text("%s", name.c_str());
                    
                    float barStart = kNameWidth + (resource.m_Lifetime.m_FirstPass - 1) * kPassWidth;
                    float barEnd = kNameWidth + (resource.m_Lifetime.m_LastPass) * kPassWidth;
                    
                    ImVec2 pMin = ImVec2(rowStart.x + barStart, rowStart.y);
                    ImVec2 pMax = ImVec2(rowStart.x + barEnd, rowStart.y + kRowHeight - 2.0f);
                    
                    drawList->AddRectFilled(pMin, pMax, color, 4.0f);
                    if (resource.m_AliasedFromIndex != UINT32_MAX)
                    {
                         drawList->AddRect(pMin, pMax, ImGui::GetColorU32(ImGuiCol_PlotLinesHovered), 4.0f, 0, 2.0f);
                    }

                    ImGui::SetCursorScreenPos(rowStart);
                    ImGui::InvisibleButton(name.c_str(), ImVec2(kNameWidth + numPasses * kPassWidth, kRowHeight));
                    
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::BeginTooltip();
                        ImGui::Text("Resource: %s", name.c_str());
                        ImGui::Text("Type: %s", typeName);
                        ImGui::Text("Size: %.2f MB", resource.m_Desc.GetMemoryRequirements().size / (1024.0 * 1024.0));
                        ImGui::Text("Lifetime: Pass %u to %u", resource.m_Lifetime.m_FirstPass, resource.m_Lifetime.m_LastPass);
                        
                        if (resource.m_Lifetime.m_FirstPass > 0 && resource.m_Lifetime.m_FirstPass <= m_PassNames.size())
                            ImGui::Text("First Pass: %s", m_PassNames[resource.m_Lifetime.m_FirstPass - 1]);
                        
                        if (resource.m_Lifetime.m_LastPass > 0 && resource.m_Lifetime.m_LastPass <= m_PassNames.size())
                            ImGui::Text("Last Pass: %s", m_PassNames[resource.m_Lifetime.m_LastPass - 1]);

                        ImGui::Text("Heap Index: %d", resource.m_HeapIndex != UINT32_MAX ? (int)resource.m_HeapIndex : -1);
                        ImGui::Text("Offset: %llu", resource.m_Offset);
                        
                        if (resource.m_AliasedFromIndex != UINT32_MAX)
                        {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 0, 1));
                            
                            std::string chain = "Aliased from " + resourceVector[resource.m_AliasedFromIndex].m_Desc.m_NvrhiDesc.debugName;
                            uint32_t nextIdx = resourceVector[resource.m_AliasedFromIndex].m_AliasedFromIndex;
                            while (nextIdx != UINT32_MAX)
                            {
                                chain += ", which was aliased from " + resourceVector[nextIdx].m_Desc.m_NvrhiDesc.debugName;
                                nextIdx = resourceVector[nextIdx].m_AliasedFromIndex;
                            }
                            
                            ImGui::TextWrapped("%s", chain.c_str());
                            ImGui::PopStyleColor();
                        }
                        else
                        {
                            ImGui::Text("Physical Owner: %s", resource.m_IsPhysicalOwner ? "Yes" : "No");
                        }
                        
                        ImGui::EndTooltip();
                    }
                };

                for (const TransientTexture& tex : m_Textures)
                    drawResourceRow(tex, "Texture", ImGui::GetColorU32(ImVec4(0.2f, 0.5f, 0.8f, 0.8f)), m_Textures);

                for (const TransientBuffer& buf : m_Buffers)
                    drawResourceRow(buf, "Buffer", ImGui::GetColorU32(ImVec4(0.2f, 0.7f, 0.3f, 0.8f)), m_Buffers);

                ImGui::EndChild();
            }
            else
            {
                ImGui::Text("No passes recorded.");
            }
            
            ImGui::TreePop();
        }
        
        if (ImGui::TreeNode("Textures"))
        {
            texFilter.Draw("Filter Textures");
            if (ImGui::BeginTable("Textures", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Dimensions");
                ImGui::TableSetupColumn("Format");
                ImGui::TableSetupColumn("First Pass");
                ImGui::TableSetupColumn("Last Pass");
                ImGui::TableSetupColumn("Memory (MB)");
                ImGui::TableSetupColumn("Heap Idx");
                ImGui::TableSetupColumn("Offset");
                ImGui::TableSetupColumn("Aliased From");
                ImGui::TableHeadersRow();
                
                for (uint32_t i = 0; i < m_Textures.size(); ++i)
                {
                    const TransientTexture& texture = m_Textures[i];
                    if (!texFilter.PassFilter(texture.m_Desc.m_NvrhiDesc.debugName.c_str()))
                        continue;
                    
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", texture.m_Desc.m_NvrhiDesc.debugName.c_str());
                    
                    ImGui::TableNextColumn();
                    ImGui::Text("%ux%u", texture.m_Desc.m_NvrhiDesc.width, texture.m_Desc.m_NvrhiDesc.height);
                    
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", nvrhi::utils::FormatToString(texture.m_Desc.m_NvrhiDesc.format));
                    
                    ImGui::TableNextColumn();
                    if (texture.m_Lifetime.IsValid())
                        ImGui::Text("%u", texture.m_Lifetime.m_FirstPass);
                    else
                        ImGui::Text("N/A");
                    
                    ImGui::TableNextColumn();
                    if (texture.m_Lifetime.IsValid())
                        ImGui::Text("%u", texture.m_Lifetime.m_LastPass);
                    else
                        ImGui::Text("N/A");
                    
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", texture.m_Desc.GetMemoryRequirements().size / (1024.0 * 1024.0));

                    ImGui::TableNextColumn();
                    ImGui::Text("%d", texture.m_HeapIndex != UINT32_MAX ? (int)texture.m_HeapIndex : -1);

                    ImGui::TableNextColumn();
                    ImGui::Text("%llu", texture.m_Offset);
                    
                    ImGui::TableNextColumn();
                    if (texture.m_AliasedFromIndex != UINT32_MAX)
                    {
                        std::string chain = m_Textures[texture.m_AliasedFromIndex].m_Desc.m_NvrhiDesc.debugName;
                        uint32_t nextIdx = m_Textures[texture.m_AliasedFromIndex].m_AliasedFromIndex;
                        while (nextIdx != UINT32_MAX)
                        {
                            chain += " -> " + m_Textures[nextIdx].m_Desc.m_NvrhiDesc.debugName;
                            nextIdx = m_Textures[nextIdx].m_AliasedFromIndex;
                        }
                        ImGui::Text("%s", chain.c_str());
                    }
                    else
                    {
                        ImGui::Text("-");
                    }
                }
                
                ImGui::EndTable();
            }
            ImGui::TreePop();
        }
        
        if (ImGui::TreeNode("Buffers"))
        {
            bufFilter.Draw("Filter Buffers");
            if (ImGui::BeginTable("Buffers", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Num Elements");
                ImGui::TableSetupColumn("Element Size");
                ImGui::TableSetupColumn("First Pass");
                ImGui::TableSetupColumn("Last Pass");
                ImGui::TableSetupColumn("Memory (MB)");
                ImGui::TableSetupColumn("Heap Idx");
                ImGui::TableSetupColumn("Offset");
                ImGui::TableSetupColumn("Aliased From");
                ImGui::TableHeadersRow();
                
                for (uint32_t i = 0; i < m_Buffers.size(); ++i)
                {
                    const TransientBuffer& buffer = m_Buffers[i];
                    if (!bufFilter.PassFilter(buffer.m_Desc.m_NvrhiDesc.debugName.c_str()))
                        continue;
                    
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", buffer.m_Desc.m_NvrhiDesc.debugName.c_str());
                    
                    ImGui::TableNextColumn();
                    if (buffer.m_Desc.m_NvrhiDesc.structStride > 0)
                        ImGui::Text("%u", buffer.m_Desc.m_NvrhiDesc.byteSize / buffer.m_Desc.m_NvrhiDesc.structStride);
                    else
                        ImGui::Text("N/A");
                    
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", buffer.m_Desc.m_NvrhiDesc.structStride);
                    
                    ImGui::TableNextColumn();
                    if (buffer.m_Lifetime.IsValid())
                        ImGui::Text("%u", buffer.m_Lifetime.m_FirstPass);
                    else
                        ImGui::Text("N/A");
                    
                    ImGui::TableNextColumn();
                    if (buffer.m_Lifetime.IsValid())
                        ImGui::Text("%u", buffer.m_Lifetime.m_LastPass);
                    else
                        ImGui::Text("N/A");
                    
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", buffer.m_Desc.GetMemoryRequirements().size / (1024.0 * 1024.0));

                    ImGui::TableNextColumn();
                    ImGui::Text("%d", buffer.m_HeapIndex != UINT32_MAX ? (int)buffer.m_HeapIndex : -1);

                    ImGui::TableNextColumn();
                    ImGui::Text("%llu", buffer.m_Offset);
                    
                    ImGui::TableNextColumn();
                    if (buffer.m_AliasedFromIndex != UINT32_MAX)
                    {
                        std::string chain = m_Buffers[buffer.m_AliasedFromIndex].m_Desc.m_NvrhiDesc.debugName;
                        uint32_t nextIdx = m_Buffers[buffer.m_AliasedFromIndex].m_AliasedFromIndex;
                        while (nextIdx != UINT32_MAX)
                        {
                            chain += " -> " + m_Buffers[nextIdx].m_Desc.m_NvrhiDesc.debugName;
                            nextIdx = m_Buffers[nextIdx].m_AliasedFromIndex;
                        }
                        ImGui::Text("%s", chain.c_str());
                    }
                    else
                    {
                        ImGui::Text("-");
                    }
                }
                
                ImGui::EndTable();
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Render Passes"))
        {
            passFilter.Draw("Filter Passes");
            uint32_t numPasses = (uint32_t)m_PassNames.size();
            if (numPasses > 0)
            {
                if (ImGui::BeginTable("RenderPassesTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
                {
                    ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 30.0f);
                    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Resources (R/W)", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                    ImGui::TableSetupColumn("Barriers", ImGuiTableColumnFlags_WidthFixed, 200.0f);
                    ImGui::TableHeadersRow();

                    for (uint32_t i = 1; i <= numPasses; ++i)
                    {
                        const char* passName = m_PassNames[i - 1];
                        const PassAccess& access = m_PassAccesses[i - 1];

                        bool match = passFilter.PassFilter(passName);
                        if (!match)
                        {
                            // Search in resources used by this pass
                            for (uint32_t idx : access.m_ReadTextures) if (passFilter.PassFilter(m_Textures[idx].m_Desc.m_NvrhiDesc.debugName.c_str())) { match = true; break; }
                            if (!match) for (uint32_t idx : access.m_WriteTextures) if (passFilter.PassFilter(m_Textures[idx].m_Desc.m_NvrhiDesc.debugName.c_str())) { match = true; break; }
                            if (!match) for (uint32_t idx : access.m_ReadBuffers) if (passFilter.PassFilter(m_Buffers[idx].m_Desc.m_NvrhiDesc.debugName.c_str())) { match = true; break; }
                            if (!match) for (uint32_t idx : access.m_WriteBuffers) if (passFilter.PassFilter(m_Buffers[idx].m_Desc.m_NvrhiDesc.debugName.c_str())) { match = true; break; }
                        }

                        if (!match)
                            continue;
                        
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Text("%u", i);

                        ImGui::TableNextColumn();
                        bool nodeOpen = ImGui::TreeNodeEx(passName, ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_FramePadding);
                        
                        ImGui::TableNextColumn();
                        ImGui::Text("T: %zu/%zu, B: %zu/%zu", 
                            access.m_ReadTextures.size(), access.m_WriteTextures.size(),
                            access.m_ReadBuffers.size(), access.m_WriteBuffers.size());
                        if (ImGui::IsItemHovered())
                        {
                            ImGui::BeginTooltip();
                            ImGui::Text("Summary of resources accessed in this pass.");
                            ImGui::EndTooltip();
                        }

                        ImGui::TableNextColumn();
                        int numBarriers = (i < m_PerPassAliasBarriers.size()) ? (int)m_PerPassAliasBarriers[i].size() : 0;
                        if (numBarriers > 0) ImGui::Text("%d barriers", numBarriers);
                        else ImGui::Text("-");

                        if (nodeOpen)
                        {
                            if (ImGui::TreeNodeEx("Resource Accesses", ImGuiTreeNodeFlags_DefaultOpen))
                            {
                                auto listAccesses = [&](const std::unordered_set<uint32_t>& indices, bool isBuffer, const char* mode, ImVec4 color) {
                                    if (indices.empty()) return;
                                    ImGui::TextColored(color, "%s %s:", mode, isBuffer ? "Buffers" : "Textures");
                                    for (uint32_t idx : indices) {
                                        const char* name = isBuffer ? m_Buffers[idx].m_Desc.m_NvrhiDesc.debugName.c_str() : m_Textures[idx].m_Desc.m_NvrhiDesc.debugName.c_str();
                                        ImGui::BulletText("%s", name);
                                    }
                                };

                                listAccesses(access.m_ReadTextures, false, "Read", ImVec4(0.4f, 0.7f, 1.0f, 1.0f));
                                listAccesses(access.m_WriteTextures, false, "Write", ImVec4(1.0f, 0.6f, 0.4f, 1.0f));
                                listAccesses(access.m_ReadBuffers, true, "Read", ImVec4(0.4f, 1.0f, 0.7f, 1.0f));
                                listAccesses(access.m_WriteBuffers, true, "Write", ImVec4(1.0f, 0.4f, 0.7f, 1.0f));
                                ImGui::TreePop();
                            }

                            if (numBarriers > 0)
                            {
                                if (ImGui::TreeNodeEx("Aliasing Barriers Details", ImGuiTreeNodeFlags_DefaultOpen))
                                {
                                    for (const AliasBarrierEntry& barrier : m_PerPassAliasBarriers[i])
                                    {
                                        const char* name = barrier.m_IsBuffer ? m_Buffers[barrier.m_ResourceIndex].m_Desc.m_NvrhiDesc.debugName.c_str() : m_Textures[barrier.m_ResourceIndex].m_Desc.m_NvrhiDesc.debugName.c_str();
                                        ImGui::BulletText("%s (%s)", name, barrier.m_IsBuffer ? "Buffer" : "Texture");
                                    }
                                    ImGui::TreePop();
                                }
                            }
                            ImGui::TreePop();
                        }
                    }
                    ImGui::EndTable();
                }
            }
            else
            {
                ImGui::Text("No passes recorded.");
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Heaps"))
        {
            heapFilter.Draw("Filter Heaps/Resources");
            for (size_t i = 0; i < m_Heaps.size(); ++i)
            {
                const HeapEntry& heap = m_Heaps[i];
                if (!heap.m_Heap) continue;

                if (ImGui::TreeNode((void*)(intptr_t)i, "Heap %zu (%.2f MB)", i, heap.m_Size / (1024.0 * 1024.0)))
                {
                    if (ImGui::BeginTable("HeapBlocks", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
                    {
                        ImGui::TableSetupColumn("Offset");
                        ImGui::TableSetupColumn("Size");
                        ImGui::TableSetupColumn("Status");
                        ImGui::TableSetupColumn("Resource");
                        ImGui::TableHeadersRow();
                        for (const HeapBlock& block : heap.m_Blocks)
                        {
                            const char* resourceName = "-";
                            if (!block.m_IsFree)
                            {
                                for (const TransientTexture& tex : m_Textures)
                                {
                                    if (tex.m_HeapIndex == (uint32_t)i && tex.m_Offset == block.m_Offset)
                                    {
                                        resourceName = tex.m_Desc.m_NvrhiDesc.debugName.c_str();
                                        break;
                                    }
                                }
                                if (resourceName[0] == '-')
                                {
                                    for (const TransientBuffer& buf : m_Buffers)
                                    {
                                        if (buf.m_HeapIndex == (uint32_t)i && buf.m_Offset == block.m_Offset)
                                        {
                                            resourceName = buf.m_Desc.m_NvrhiDesc.debugName.c_str();
                                            break;
                                        }
                                    }
                                }
                            }

                            if (!heapFilter.PassFilter(resourceName) && !heapFilter.PassFilter(block.m_IsFree ? "Free" : "Allocated"))
                                continue;

                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::Text("%llu", block.m_Offset);
                            ImGui::TableNextColumn();
                            ImGui::Text("%.2f KB", block.m_Size / 1024.0);
                            ImGui::TableNextColumn();
                            ImGui::Text("%s", block.m_IsFree ? "Free" : "Allocated");
                            ImGui::TableNextColumn();
                            ImGui::Text("%s", resourceName);
                        }
                        ImGui::EndTable();
                    }
                    ImGui::TreePop();
                }
            }
            ImGui::TreePop();
        }
        
        ImGui::TreePop();
    }
}