// TestMain.cpp — doctest implementation unit + test runner entry point
//
// DOCTEST_CONFIG_IMPLEMENT must be defined in exactly ONE translation unit.
// This file is that unit. All other test files include TestFixtures.h which
// does NOT define DOCTEST_CONFIG_IMPLEMENT.
// ============================================================================

#define DOCTEST_CONFIG_IMPLEMENT
#include "../external/doctest.h"

#include "../Renderer.h"
#include "../Config.h"

// ============================================================================
// RunTests — called from Renderer.cpp main() when --run-tests is detected.
// ============================================================================
int RunTests(int argc, char* argv[])
{
    // Config::ParseCommandLine (used by Config tests) calls Renderer::GetInstance().
    // Set up a minimal Renderer instance so the assert doesn't fire.
    // The renderer is NOT initialized (no GPU/RHI) — it is only used as a
    // target for the few ParseCommandLine paths that write to renderer fields.

    doctest::Context ctx;

    // Forward all arguments so --run-tests=Pattern and --verbose work.
    std::vector<const char*> doctestArgv;
    doctestArgv.push_back(argv[0]); // program name

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];

        // --run-tests=Pattern  → pass "-tc=Pattern" to doctest
        if (arg.starts_with("--run-tests="))
        {
            std::string filter = "-tc=";
            filter += arg.substr(std::string_view("--run-tests=").size());
            static std::string s_filter;
            s_filter = std::move(filter);
            doctestArgv.push_back(s_filter.c_str());
        }
        // --verbose → doctest -s (success output)
        else if (arg == "--verbose")
        {
            doctestArgv.push_back("-s");
        }
        // Pass through any other --dt-* doctest native flags
        else if (arg.starts_with("--dt-"))
        {
            doctestArgv.push_back(argv[i]);
        }
        // Skip --run-tests (bare, no pattern)
        else if (arg == "--run-tests")
        {
            // nothing
        }
    }

    ctx.applyCommandLine(static_cast<int>(doctestArgv.size()),
                         const_cast<char**>(doctestArgv.data()));

    ctx.setOption("no-intro", true);   // suppress doctest banner
    ctx.setOption("no-version", true);

    SDL_Log("[Tests] Starting test run (%d doctest arg(s))",
            static_cast<int>(doctestArgv.size()) - 1);

    const int result = ctx.run();

    if (ctx.shouldExit())
        return result;

    return result;
}
