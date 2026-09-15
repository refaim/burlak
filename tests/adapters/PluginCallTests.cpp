#include "adapters/far/PluginCall.hpp"

#include <doctest/doctest.h>

#include <windows.h>

#include <plugin.hpp>

#include <array>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <vector>

namespace burlak::adapters::far_api
{

    namespace
    {

        void WINAPI freeItem(void *, const FarPanelItemFreeInfo *)
        {
        }

        core::Item nativeItem(std::wstring_view name, std::uintptr_t attributes = 0,
                              PLUGINPANELITEMFLAGS flags = PPIF_NONE, bool detailed = false)
        {
            constexpr std::array details{std::wstring_view{L"ONE.TXT"}, std::wstring_view{L"description"},
                                         std::wstring_view{L"owner"}, std::wstring_view{L"column-a"},
                                         std::wstring_view{L"column-b"}};
            std::size_t stringBytes = (name.size() + 1) * sizeof(wchar_t);
            if (detailed) {
                for (const auto text : details) {
                    stringBytes += (text.size() + 1) * sizeof(wchar_t);
                }
            }
            core::Item result{.name = std::wstring{name},
                              .size = detailed ? 19U : 0U,
                              .attributes = attributes,
                              .directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                              .selected = (flags & PPIF_SELECTED) != 0,
                              .userData = {.value = detailed ? 23U : 0U}};
            const std::size_t columnBytes = detailed ? 3 * sizeof(const wchar_t *) : 0;
            result.native.resize(sizeof(PluginPanelItem) + columnBytes + stringBytes);
            auto &native = *reinterpret_cast<PluginPanelItem *>(result.native.data());
            native = {};
            auto *cursor = result.native.data() + sizeof(PluginPanelItem) + columnBytes;
            const auto write = [&cursor](std::wstring_view text) {
                auto *destination = reinterpret_cast<wchar_t *>(cursor);
                std::memcpy(destination, text.data(), text.size() * sizeof(wchar_t));
                destination[text.size()] = L'\0';
                cursor += (text.size() + 1) * sizeof(wchar_t);
                return static_cast<const wchar_t *>(destination);
            };
            native.FileName = write(name);
            native.FileAttributes = attributes;
            native.Flags = flags;
            if (detailed) {
                auto **columns = reinterpret_cast<const wchar_t **>(result.native.data() + sizeof(PluginPanelItem));
                native.CreationTime = {1, 2};
                native.LastAccessTime = {3, 4};
                native.LastWriteTime = {5, 6};
                native.ChangeTime = {7, 8};
                native.FileSize = 19;
                native.AllocationSize = 31;
                native.AlternateFileName = write(details[0]);
                native.Description = write(details[1]);
                native.Owner = write(details[2]);
                columns[0] = write(details[3]);
                columns[1] = write(details[4]);
                columns[2] = nullptr;
                native.CustomColumnData = columns;
                native.CustomColumnNumber = 3;
                native.UserData = {reinterpret_cast<void *>(23), freeItem};
                native.NumberOfLinks = 5;
                native.CRC32 = 0xabcdef;
                native.Reserved[0] = 29;
                native.Reserved[1] = 31;
            }
            return result;
        }

    } // namespace

    TEST_SUITE("plugin call")
    {
        TEST_CASE("GetFilesW receives the panel, items, destination, and plugin instance")
        {
            const auto destination = std::filesystem::temp_directory_path() / L"burlak-getfiles-success";
            std::filesystem::remove_all(destination);
            std::vector<core::Item> items;
            items.push_back(nativeItem(L"one.txt", FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM,
                                       PPIF_SELECTED | PPIF_PROCESSDESCR, true));
            items.push_back(nativeItem(L"two.txt"));
            items.push_back(nativeItem(L"folder", FILE_ATTRIBUTE_DIRECTORY, PPIF_SELECTED));
            const core::PluginModule module{.path = GETFILES_SUCCESS_PATH, .instance = 43};

            CHECK(callPluginGetFiles(7, items, module, destination.wstring()) == destination.wstring());
            CHECK(std::filesystem::exists(destination / L"one.txt"));
            CHECK(std::filesystem::exists(destination / L"two.txt"));
            CHECK(std::filesystem::is_directory(destination / L"folder"));

            const core::PluginModule wrongInstance{.path = GETFILES_SUCCESS_PATH, .instance = 42};
            CHECK(callPluginGetFiles(7, items, wrongInstance, destination.wstring()).error() ==
                  core::Error::ForeignCallFailed);

            const core::PluginModule alternateSuccess{.path = GETFILES_SUCCESS_PATH, .instance = 2};
            std::vector<core::Item> alternateItems;
            alternateItems.push_back(nativeItem(L"alternate.txt"));
            CHECK(callPluginGetFiles(7, alternateItems, alternateSuccess, destination.wstring()) ==
                  destination.wstring());

            const core::PluginModule rewritesDestination{.path = GETFILES_SUCCESS_PATH, .instance = 45};
            const auto rewritten = callPluginGetFiles(7, alternateItems, rewritesDestination, destination.wstring());
            REQUIRE(rewritten.has_value());
            CHECK(*rewritten == destination.wstring() + L"-rewritten");
            CHECK(std::filesystem::exists(std::filesystem::path{*rewritten} / L"alternate.txt"));
            std::filesystem::remove_all(*rewritten);

            const core::PluginModule clearsDestination{.path = GETFILES_SUCCESS_PATH, .instance = 46};
            CHECK(callPluginGetFiles(7, alternateItems, clearsDestination, destination.wstring()).error() ==
                  core::Error::ForeignCallFailed);

            const core::PluginModule invalidDestination{.path = GETFILES_SUCCESS_PATH, .instance = 47};
            CHECK(callPluginGetFiles(7, alternateItems, invalidDestination, destination.wstring()).error() ==
                  core::Error::ForeignCallCrashed);

            const core::PluginModule overlongDestination{.path = GETFILES_SUCCESS_PATH, .instance = 48};
            CHECK(callPluginGetFiles(7, alternateItems, overlongDestination, destination.wstring()).error() ==
                  core::Error::ForeignCallFailed);
            std::filesystem::remove_all(destination);
        }

        TEST_CASE("a missing export, plugin refusal, and access violation are expected failures")
        {
            const auto destination = std::filesystem::temp_directory_path() / L"burlak-getfiles-failure";
            std::vector<core::Item> items;
            items.push_back(nativeItem(L"one.txt"));

            const core::PluginModule missing{.path = L"missing-burlak-plugin.dll", .instance = 0};
            CHECK(callPluginGetFiles(0, items, missing, destination.wstring()).error() ==
                  core::Error::ForeignCallFailed);

            const core::PluginModule noExport{.path = L"kernel32.dll", .instance = 0};
            CHECK(callPluginGetFiles(0, items, noExport, destination.wstring()).error() ==
                  core::Error::ForeignCallFailed);

            const std::vector<core::Item> missingNative{{.name = L"alternate.txt"}};
            const core::PluginModule callable{.path = GETFILES_SUCCESS_PATH, .instance = 2};
            CHECK(callPluginGetFiles(0, missingNative, callable, destination.wstring()).error() ==
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
