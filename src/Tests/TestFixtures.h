#pragma once

// ============================================================================
// TestFixtures.h — Shared test infrastructure for HobbyRenderer test suite
//
// This header is included by ALL test files (Tests_CoreBoot.cpp, etc.).
// It does NOT define DOCTEST_CONFIG_IMPLEMENT — that is done exactly once
// in TestMain.cpp.
//
// Usage:
//   #include "TestFixtures.h"
//   TEST_CASE("MyTest") { CHECK(1 + 1 == 2); }
// ============================================================================

// doctest.h without the implementation macro — just the test macros.
#include "../external/doctest.h"

#include "../CommonResources.h"
#include "../Config.h"
#include "../GraphicRHI.h"
#include "../Renderer.h"
#include "../TaskScheduler.h"
#include "../Utilities.h"

// Convenience alias
static CommonResources& CR() { return CommonResources::GetInstance(); }
static nvrhi::IDevice*  DEV() { return g_Renderer.m_RHI->m_NvrhiDevice; }

// ============================================================================
// Helper: tiny RAII wrapper that resets Config to defaults after a test
// ============================================================================
struct ConfigGuard
{
    // Snapshot the current singleton state
    Config snapshot = Config::Get();

    ~ConfigGuard()
    {
        // Restore via the private s_Instance — we access it through ParseCommandLine
        // with a no-op argv so we just re-assign the snapshot directly.
        // Since Config::s_Instance is inline and accessible via the header we
        // reach it through the public Get() reference cast.
        const_cast<Config&>(Config::Get()) = snapshot;
    }
};
