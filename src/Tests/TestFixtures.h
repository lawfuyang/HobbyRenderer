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

#include "../TaskScheduler.h"
#include "../Config.h"
#include "../Utilities.h"
#include "../Renderer.h"
