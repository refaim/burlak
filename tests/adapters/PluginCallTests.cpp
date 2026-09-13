#include "adapters/far/PluginCall.hpp"

#include <doctest/doctest.h>

#include <filesystem>

namespace burlak::adapters::far_api
{

    TEST_SUITE("plugin call")
    {
        TEST_CASE("GetFilesW receives the panel, items, destination, and plugin instance")
        {
            const auto destination = std::filesystem::temp_directory_path() / L"burlak-getfiles-success";
            std::filesystem::remove_all(destination);
            const std::vector<core::Item> items{
                {.name = L"one.txt"}, {.name = L"two.txt"}, {.name = L"folder", .directory = true}};
            const core::PluginModule module{.path = GETFILES_SUCCESS_PATH, .instance = 42};

            CHECK(callPluginGetFiles(7, items, module, destination.wstring()).has_value());
            CHECK(std::filesystem::exists(destination / L"one.txt"));
            CHECK(std::filesystem::exists(destination / L"two.txt"));
            CHECK(std::filesystem::is_directory(destination / L"folder"));

            const core::PluginModule alternateSuccess{.path = GETFILES_SUCCESS_PATH, .instance = 2};
            CHECK(callPluginGetFiles(7, items, alternateSuccess, destination.wstring()).has_value());
            std::filesystem::remove_all(destination);
        }

        TEST_CASE("a missing export, plugin refusal, and access violation are expected failures")
        {
            const auto destination = std::filesystem::temp_directory_path() / L"burlak-getfiles-failure";
            const std::vector<core::Item> items{{.name = L"one.txt"}};

            const core::PluginModule missing{.path = L"missing-burlak-plugin.dll", .instance = 0};
            CHECK(callPluginGetFiles(0, items, missing, destination.wstring()).error() ==
                  core::Error::ForeignCallFailed);

            const core::PluginModule noExport{.path = L"kernel32.dll", .instance = 0};
            CHECK(callPluginGetFiles(0, items, noExport, destination.wstring()).error() ==
                  core::Error::ForeignCallFailed);

            const core::PluginModule refusal{.path = GETFILES_SUCCESS_PATH, .instance = 13};
            CHECK(callPluginGetFiles(0, items, refusal, destination.wstring()).error() ==
                  core::Error::ForeignCallFailed);

            const core::PluginModule crash{.path = GETFILES_CRASH_PATH, .instance = 0};
            CHECK(callPluginGetFiles(0, items, crash, destination.wstring()).error() ==
                  core::Error::ForeignCallCrashed);
        }
    }

} // namespace burlak::adapters::far_api
