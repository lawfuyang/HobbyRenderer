#pragma once

#include "Scene.h"

// Aggregated cluster-LOD statistics of a loaded scene, plus the CPU-side correctness gates of phase 0
// (docs/cluster-lod-implementation-plan.md V0.1-V0.3) run over every primitive of it.
//
// `--cluster-stats` calls this once after the scene is loaded. The primitive geometry is rebuilt from the level-0
// meshlets, so it needs no CPU index buffer to still be alive.
void LogSceneClusterStats(const Scene& scene);

// The CPU reference for V1.1: how many clusters ClusterLod::EvaluateCut draws for `bucket` at this camera. The
// GPU traversal applies the cut rule per group, so with culling off the two counts have to match exactly.
uint32_t CountReferenceCutClusters(const Scene& scene, const Scene::BucketInfo& bucket, const ClusterLod::CutParams& baseParams);
