#include "Renderer.h"

// Resolves the visibility buffer into the G-buffer. The resource and the schedule land first: with --vis-buffer 1
// the pass is ordered between the masked bucket and the HZB, declares nothing and draws nothing, so the frame is
// identical to the raster path (milestone M1). The resolve pixel shader replaces the empty body.
class VisibilityResolveRenderer : public IRenderer
{
public:
    bool Setup(RenderGraph&) override
    {
        // Nothing reads or writes a graph resource until the resolve exists. A pass with no declarations is still
        // scheduled, which is what the VisibilityResolve capture marker reports on.
        return g_Renderer.m_EnableVisibilityBuffer;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph&) override
    {
        PROFILE_GPU_SCOPED("VisibilityResolve", commandList);
        // The resolve binds the visibility buffer and the cluster job/record/stream buffers and writes the six
        // G-buffer targets. Until then, the marker is the whole pass.
    }

    const char* GetName() const override { return "Visibility Resolve"; }
};

REGISTER_RENDERER(VisibilityResolveRenderer);
