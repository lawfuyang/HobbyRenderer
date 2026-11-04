#pragma once

class Scene;

// Uploads every baked micromap array and builds it on the GPU. Has to be recorded before the BLAS builds,
// which link the finished arrays into their geometry. LODs whose bake produced no masks are skipped.
void BuildSceneOmm(Scene& scene);
