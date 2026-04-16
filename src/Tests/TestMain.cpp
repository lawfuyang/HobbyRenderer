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
#include "../Utilities.h"

// ============================================================================
// TeeStreambuf — fans every write to three sinks simultaneously:
//   1. std::cout  (stdout)
//   2. OutputDebugStringA  (VS Output window / debugger)
//   3. a file  (tests_output.txt next to the .exe)
// ============================================================================
class TeeStreambuf : public std::streambuf
{
public:
    explicit TeeStreambuf(const std::filesystem::path& filePath)
    {
        m_file.open(filePath, std::ios::out | std::ios::trunc);
        if (!m_file.is_open())
            SDL_Log("[Tests] WARNING: could not open output file: %s",
                    filePath.string().c_str());
    }

    ~TeeStreambuf() override
    {
        if (m_file.is_open())
            m_file.close();
    }

protected:
    // Called for each character (or small burst) written to the stream.
    int overflow(int c) override
    {
        if (c == EOF)
            return EOF;

        const char ch = static_cast<char>(c);

        // 1. stdout
        std::cout.put(ch);

        // 2. file
        if (m_file.is_open())
            m_file.put(ch);

        // 3. OutputDebugStringA — accumulate into a line buffer and flush on '\n'
        m_debugLine += ch;
        if (ch == '\n')
            FlushDebugLine();

        return c;
    }

    // Called when the stream flushes.
    int sync() override
    {
        std::cout.flush();
        if (m_file.is_open())
            m_file.flush();
        if (!m_debugLine.empty())
            FlushDebugLine();
        return 0;
    }

    // Called for bulk writes — override for efficiency.
    std::streamsize xsputn(const char* s, std::streamsize count) override
    {
        // 1. stdout
        std::cout.write(s, count);

        // 2. file
        if (m_file.is_open())
            m_file.write(s, count);

        // 3. OutputDebugStringA — accumulate and flush on newlines
        for (std::streamsize i = 0; i < count; ++i)
        {
            m_debugLine += s[i];
            if (s[i] == '\n')
                FlushDebugLine();
        }

        return count;
    }

private:
    void FlushDebugLine()
    {
        if (!m_debugLine.empty())
        {
            OutputDebugStringA(m_debugLine.c_str());
            m_debugLine.clear();
        }
    }

    std::ofstream m_file;
    std::string   m_debugLine; // accumulates until '\n' for OutputDebugStringA
};

// ============================================================================
// TeeStream — std::ostream backed by TeeStreambuf.
// ============================================================================
class TeeStream : public std::ostream
{
public:
    explicit TeeStream(const std::filesystem::path& filePath)
        : std::ostream(nullptr), m_buf(filePath)
    {
        rdbuf(&m_buf);
    }

private:
    TeeStreambuf m_buf;
};

// ============================================================================
// SDLLogListener — a doctest LISTENER that mirrors key events to SDL_Log.
// This ensures the SDL logging system (and any SDL log callbacks) also
// receives test output, in addition to the TeeStream above.
// ============================================================================
struct SDLLogListener : public doctest::IReporter
{
    const doctest::ContextOptions& opt;
    const doctest::TestCaseData*   tc = nullptr;
    std::mutex                     mutex;

    explicit SDLLogListener(const doctest::ContextOptions& in) : opt(in) {}

    void report_query(const doctest::QueryData&) override {}

    void test_run_start() override
    {
        SDL_Log("[Tests] ===== Test run started =====");
    }

    void test_run_end(const doctest::TestRunStats& stats) override
    {
        SDL_Log("[Tests] ===== Test run finished =====");
        SDL_Log("[Tests]   Total test cases : %u", stats.numTestCases);
        SDL_Log("[Tests]   Passed           : %u", stats.numTestCasesPassingFilters - stats.numTestCasesFailed);
        SDL_Log("[Tests]   Failed           : %u", stats.numTestCasesFailed);
        SDL_Log("[Tests]   Total asserts    : %d", stats.numAsserts);
        SDL_Log("[Tests]   Failed asserts   : %d", stats.numAssertsFailed);
    }

    void test_case_start(const doctest::TestCaseData& in) override
    {
        tc = &in;
        SDL_Log("[Tests] [ RUN  ] %s", in.m_name);
    }

    void test_case_reenter(const doctest::TestCaseData&) override {}

    void test_case_end(const doctest::CurrentTestCaseStats& stats) override
    {
        if (tc)
        {
            if (stats.failure_flags == 0)
                SDL_Log("[Tests] [  OK  ] %s", tc->m_name);
            else
                SDL_Log("[Tests] [ FAIL ] %s", tc->m_name);
        }
    }

    void test_case_exception(const doctest::TestCaseException& ex) override
    {
        SDL_Log("[Tests] [EXCEPTION] %s", ex.error_string.c_str());
    }

    void subcase_start(const doctest::SubcaseSignature&) override {}
    void subcase_end() override {}

    void log_assert(const doctest::AssertData& in) override
    {
        if (!in.m_failed && !opt.success)
            return;

        const std::lock_guard<std::mutex> lock(mutex);

        if (in.m_failed)
        {
            SDL_Log("[Tests] ASSERT FAILED  %s(%d): %s",
                    in.m_file, in.m_line, in.m_expr);
        }
    }

    void log_message(const doctest::MessageData&) override {}

    void test_case_skipped(const doctest::TestCaseData& in) override
    {
        SDL_Log("[Tests] [SKIP  ] %s", in.m_name);
    }
};

// Register as a LISTENER (priority 1) so it runs alongside the default console reporter.
REGISTER_LISTENER("sdl_log_listener", 1, SDLLogListener);

// ============================================================================
// Assert handler — called by doctest when an assertion fails.
// Uses SDL_assert so the debugger breaks at the failure site.
// ============================================================================
static void DoctestAssertHandler(const doctest::AssertData& ad)
{
    if (ad.m_failed)
    {
        // SDL_assert triggers a breakpoint in debug builds and logs the failure.
        // We format the message manually because SDL_assert takes a literal.
        SDL_Log("[Tests] ASSERT FAILED %s(%d): CHECK( %s )",
                ad.m_file, ad.m_line, ad.m_expr);

        // SDL_assert(false) will break into the debugger on debug builds.
        SDL_assert(false && "doctest assertion failed — see log above");
    }
}

// ============================================================================
// Helper: resolve the path to tests_output.txt next to the running .exe
// ============================================================================
static std::filesystem::path GetTestOutputFilePath()
{
    // SDL_GetBasePath() returns the directory containing the executable.
    const char* basePath = SDL_GetBasePath();
    if (basePath)
        return std::filesystem::path(basePath) / "tests_output.txt";

    // Fallback: write next to the current working directory
    return std::filesystem::current_path() / "tests_output.txt";
}

void SDLCALL LogTestsSDLOutputFunction(void* userdata, int category, SDL_LogPriority priority, const char* message)
{
    FILE* f = (FILE*)userdata;
    std::string strWithNewLine = std::string(message) + "\n";
    printf("%s", strWithNewLine.c_str()); // also print to console for real-time visibility
    ::OutputDebugStringA(strWithNewLine.c_str()); // also send to debugger output on Windows
    fprintf(f, "%s", strWithNewLine.c_str()); // write to file to capture the full test output for diagnostics
    fflush(f);
}

// ============================================================================
// RunTests — called from Renderer.cpp main() when --run-tests is detected.
// ============================================================================
int RunTests(int argc, char* argv[])
{
    SimpleTimer timer;

    // ------------------------------------------------------------------
    // Set up the tee output stream: stdout + OutputDebugStringA + file
    // ------------------------------------------------------------------
    const std::filesystem::path outputFilePath = GetTestOutputFilePath();
    FILE* logfile = fopen(outputFilePath.string().c_str(), "w");
    std::unique_ptr<FILE, decltype(&fclose)> fileGuard{ logfile, &fclose };

    SDL_SetLogOutputFunction(LogTestsSDLOutputFunction, logfile);
    TeeStream teeStream(outputFilePath);

    SDL_Log("[Tests] ========== TEST EXECUTION START ==========");
    {
        auto now = std::chrono::system_clock::now();
        std::time_t now_c = std::chrono::system_clock::to_time_t(now);
        SDL_Log("[TEST_RUNNER] Start time: %s", std::ctime(&now_c));
    }
    
    SDL_Log("[Tests] Output file: %s", outputFilePath.string().c_str());

    // ------------------------------------------------------------------
    // Build doctest context
    // ------------------------------------------------------------------
    doctest::Context ctx;

    // Redirect all doctest text output through our tee stream.
    ctx.setCout(&teeStream);

    // Break into the debugger on assertion failures via SDL_assert.
    ctx.setAssertHandler(DoctestAssertHandler);

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

    SDL_Log("[Tests] Total execution time: %.2fs", timer.TotalSeconds());

    if (ctx.shouldExit())
        return result;

    return result;
}
