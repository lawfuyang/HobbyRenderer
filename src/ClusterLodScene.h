#pragma once

#include "Scene.h"

// Aggregated cluster-LOD statistics of a loaded scene, plus the CPU-side correctness gates of phase 0
// (docs/cluster-lod-implementation-plan.md V0.1-V0.3) run over every primitive of it.
//
// `--cluster-stats` calls this once after the scene is loaded. The primitive geometry is rebuilt from the level-0
// meshlets, so it needs no CPU index buffer to still be alive.
void LogSceneClusterStats(const Scene& scene);
