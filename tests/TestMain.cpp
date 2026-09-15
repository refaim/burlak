#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <windows.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>

namespace
{

    // Diagnostics for a process that dies by abort()/std::terminate: the runner shows only the exit code
    // (0xC0000409, the UCRT's fast-fail after abort) because ctest captures stdout through a pipe that is never
    // flushed on that path. The name of the running test case is kept here, doctest's own output is flushed after
    // every failure, and the abort hook prints both before the process goes down.
    char currentTestCase[512]{};

    void describeActiveException()
    {
        const auto active = std::current_exception();
        if (!active) {
            std::fputs("[doctest] active exception: none (abort without a C++ exception)\n", stderr);
            return;
        }
        try {
            std::rethrow_exception(active);
        } catch (const std::exception &error) {
            std::fprintf(stderr, "[doctest] active exception: %s\n", error.what());
        } catch (...) {
            std::fputs("[doctest] active exception: not derived from std::exception\n", stderr);
        }
    }

    void reportFatalExit(const char *reason)
    {
        std::fprintf(stderr, "[doctest] %s in test case: %s\n", reason, currentTestCase);
        describeActiveException();
        std::fflush(stderr);
        std::cout.flush();
        std::fflush(stdout);
    }

    // The UCRT keeps the SIGABRT handler process-wide, so an abort on any thread (an unhandled exception in a
    // thread body, std::terminate) reaches it; MSVC's std::set_terminate is per thread and covers only the main
    // thread here.
    void onAbort(int)
    {
        reportFatalExit("abort");
    }

    void onTerminate()
    {
        reportFatalExit("std::terminate");
        std::abort();
    }

    [[maybe_unused]] const int abortHookInstalled = [] {
        static_cast<void>(std::signal(SIGABRT, onAbort));
        static_cast<void>(std::set_terminate(onTerminate));
        return 0;
    }();

    class TraceTests final : public doctest::IReporter
    {
      public:
        explicit TraceTests(const doctest::ContextOptions &)
        {
            wchar_t value[2]{};
            enabled_ = GetEnvironmentVariableW(L"BURLAK_TRACE_TESTS", value, 2) == 1 && value[0] == L'1';
        }

        void report_query(const doctest::QueryData &) override
        {
        }
        void test_run_start() override
        {
        }
        void test_run_end(const doctest::TestRunStats &) override
        {
        }
        void test_case_start(const doctest::TestCaseData &test) override
        {
            std::snprintf(currentTestCase, sizeof(currentTestCase), "%s", test.m_name);
            if (enabled_) {
                std::fprintf(stderr, "[doctest] starting: %s\n", test.m_name);
                std::fflush(stderr);
            }
        }
        void test_case_reenter(const doctest::TestCaseData &) override
        {
        }
        void test_case_end(const doctest::CurrentTestCaseStats &) override
        {
            std::cout.flush();
        }
        void test_case_exception(const doctest::TestCaseException &) override
        {
            std::cout.flush();
        }
        void subcase_start(const doctest::SubcaseSignature &) override
        {
        }
        void subcase_end() override
        {
        }
        void log_assert(const doctest::AssertData &assertion) override
        {
            if (assertion.m_failed) {
                std::cout.flush();
            }
        }
        void log_message(const doctest::MessageData &) override
        {
            std::cout.flush();
        }
        void test_case_skipped(const doctest::TestCaseData &) override
        {
        }

      private:
        bool enabled_{};
    };

    DOCTEST_REGISTER_LISTENER("trace-tests", 1, TraceTests);

} // namespace
