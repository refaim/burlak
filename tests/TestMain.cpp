#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <windows.h>

#include <cstdio>

namespace
{

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
        }
        void test_case_exception(const doctest::TestCaseException &) override
        {
        }
        void subcase_start(const doctest::SubcaseSignature &) override
        {
        }
        void subcase_end() override
        {
        }
        void log_assert(const doctest::AssertData &) override
        {
        }
        void log_message(const doctest::MessageData &) override
        {
        }
        void test_case_skipped(const doctest::TestCaseData &) override
        {
        }

      private:
        bool enabled_{};
    };

    DOCTEST_REGISTER_LISTENER("trace-tests", 1, TraceTests);

} // namespace
