// GraphicsTestUtils.h — Stub helpers for graphics validation (Phase 6+)
//
// These functions are stubs that always succeed. They will be fleshed out
// in a later phase to perform real backbuffer capture and image comparison.
// ============================================================================

#pragma once

#include "../pch.h"

// ============================================================================
// Stub: capture the current backbuffer to a BMP file.
// Returns true (no-op until Phase 6 implementation).
// ============================================================================
inline bool capture_backbuffer_to_bmp(const char* /*outputPath*/)
{
    // TODO (Phase 6): read backbuffer via staging texture, encode to BMP, write file.
    return true;
}

// ============================================================================
// Stub: compare two BMP images using a fuzzy/perceptual metric.
// Returns true (no-op until Phase 6 implementation).
// ============================================================================
inline bool compare_bmp_images(const char* /*actualPath*/, const char* /*referencePath*/,
                                float /*tolerancePercent*/ = 1.0f)
{
    // TODO (Phase 6): load both BMPs, compute SSIM or per-pixel distance, return pass/fail.
    return true;
}
