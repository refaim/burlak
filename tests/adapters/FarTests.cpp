#include "adapters/far/FarApi.hpp"

#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <string>

namespace burlak::adapters::far_api
{

    namespace
    {

        int panelInfoCalls{};
        int updateCalls{};
        int redrawCalls{};
        int synchroCalls{};
        int messageCalls{};
        bool panelsWindow{true};
        bool advFailure{};
        std::wstring directoryName{L"C:\\panel"};
        std::wstring selectedName{L"selected.txt"};
        PANELINFOTYPE panelType{PTYPE_FILEPANEL};
        UUID ownerGuid{0x12345678, 0x1111, 0x2222, {1, 2, 3, 4, 5, 6, 7, 8}};
        GlobalInfo pluginGlobal{};
        PluginInfo pluginInfo{};
        std::wstring pluginPath{L"C:\\plugins\\Sample.dll"};

        enum class PanelFailure : std::uint8_t
        {
            None,
            Info,
            DirectorySize,
            DirectoryFill,
            DirectoryName,
            ItemSize,
            ItemFill,
            ItemName
        };

        enum class PluginFailure : std::uint8_t
        {
            None,
            Find,
            Size,
            Fill,
            ModuleName,
            GlobalInfo
        };

        PanelFailure panelFailure{PanelFailure::None};
        PluginFailure pluginFailure{PluginFailure::None};

        intptr_t WINAPI panelControl(HANDLE panel, FILE_CONTROL_COMMANDS command, intptr_t param1, void *param2)
        {
            if (command == FCTL_GETPANELINFO) {
                ++panelInfoCalls;
                if (panelFailure == PanelFailure::Info) {
                    return 0;
                }
                auto &info = *static_cast<PanelInfo *>(param2);
                info.PanelType = panelType;
                info.Flags = PFLAGS_VISIBLE | PFLAGS_REALNAMES | (panel == PANEL_PASSIVE ? PFLAGS_PLUGIN : PFLAGS_NONE);
                info.PanelRect = panel == PANEL_ACTIVE ? RECT{0, 0, 39, 24} : RECT{40, 0, 79, 24};
                info.PluginHandle = reinterpret_cast<HANDLE>(17);
                info.OwnerGuid = ownerGuid;
                info.SelectedItemsNumber = 1;
                info.CurrentItem = 7;
                info.TopPanelItem = 3;
                return 1;
            }
            if (command == FCTL_GETPANELDIRECTORY) {
                const auto bytes = sizeof(FarPanelDirectory) + (directoryName.size() + 1) * sizeof(wchar_t);
                if (param2 == nullptr) {
                    return panelFailure == PanelFailure::DirectorySize ? 0 : static_cast<intptr_t>(bytes);
                }
                if (panelFailure == PanelFailure::DirectoryFill) {
                    return 0;
                }
                auto &directory = *static_cast<FarPanelDirectory *>(param2);
                auto *name = reinterpret_cast<wchar_t *>(static_cast<std::byte *>(param2) + sizeof(FarPanelDirectory));
                std::memcpy(name, directoryName.c_str(), (directoryName.size() + 1) * sizeof(wchar_t));
                directory.Name = panelFailure == PanelFailure::DirectoryName ? nullptr : name;
                return static_cast<intptr_t>(bytes);
            }
            if (command == FCTL_GETSELECTEDPANELITEM) {
                const auto bytes = sizeof(PluginPanelItem) + (selectedName.size() + 1) * sizeof(wchar_t);
                if (param2 == nullptr) {
                    return panelFailure == PanelFailure::ItemSize ? 0 : static_cast<intptr_t>(bytes);
                }
                if (panelFailure == PanelFailure::ItemFill) {
                    return 0;
                }
                auto &request = *static_cast<FarGetPluginPanelItem *>(param2);
                *request.Item = {};
                auto *name =
                    reinterpret_cast<wchar_t *>(reinterpret_cast<std::byte *>(request.Item) + sizeof(PluginPanelItem));
                std::memcpy(name, selectedName.c_str(), (selectedName.size() + 1) * sizeof(wchar_t));
                request.Item->FileName = panelFailure == PanelFailure::ItemName ? nullptr : name;
                request.Item->FileSize = 9;
                request.Item->FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
                request.Item->UserData.Data = reinterpret_cast<void *>(23);
                return static_cast<intptr_t>(bytes);
            }
            if (command == FCTL_UPDATEPANEL) {
                ++updateCalls;
                return 1;
            }
            if (command == FCTL_REDRAWPANEL) {
                ++redrawCalls;
                return 1;
            }
            static_cast<void>(param1);
            return 0;
        }

        intptr_t WINAPI advControl(const UUID *, ADVANCED_CONTROL_COMMANDS command, intptr_t, void *param2)
        {
            if (command == ACTL_SYNCHRO) {
                ++synchroCalls;
                return 1;
            }
            if (advFailure) {
                return 0;
            }
            auto &info = *static_cast<WindowInfo *>(param2);
            info.Type = panelsWindow ? WTYPE_PANELS : WTYPE_EDITOR;
            return 1;
        }

        intptr_t WINAPI message(const UUID *, const UUID *, FARMESSAGEFLAGS, const wchar_t *, const wchar_t *const *,
                                std::size_t, intptr_t)
        {
            ++messageCalls;
            return 0;
        }

        intptr_t WINAPI pluginsControl(HANDLE, FAR_PLUGINS_CONTROL_COMMANDS command, intptr_t, void *param2)
        {
            if (command == PCTL_FINDPLUGIN) {
                return pluginFailure == PluginFailure::Find ? 0 : 91;
            }
            if (param2 == nullptr) {
                return pluginFailure == PluginFailure::Size ? 0 : sizeof(FarGetPluginInformation);
            }
            if (pluginFailure == PluginFailure::Fill) {
                return 0;
            }
            auto &information = *static_cast<FarGetPluginInformation *>(param2);
            information.ModuleName = pluginFailure == PluginFailure::ModuleName ? nullptr : pluginPath.c_str();
            information.GInfo = pluginFailure == PluginFailure::GlobalInfo ? nullptr : &pluginGlobal;
            information.PInfo = &pluginInfo;
            return sizeof(FarGetPluginInformation);
        }

        PluginStartupInfo startup()
        {
            PluginStartupInfo info{};
            info.StructSize = sizeof(info);
            info.PanelControl = panelControl;
            info.AdvControl = advControl;
            info.Message = message;
            info.PluginsControl = pluginsControl;
            return info;
        }

    } // namespace

    TEST_SUITE("Far adapters")
    {
        TEST_CASE("panels map Far panel data to core values")
        {
            panelFailure = PanelFailure::None;
            advFailure = false;
            updateCalls = 0;
            redrawCalls = 0;
            auto info = startup();
            FarPanels panels{info};

            const auto active = panels.panel(core::PanelSide::Active);
            REQUIRE(active.has_value());
            CHECK(active->visible);
            CHECK(active->realNames);
            CHECK_FALSE(active->plugin);
            CHECK(active->filePanel);
            CHECK(active->rect == core::CellRect{0, 0, 39, 24});
            CHECK(active->handle == 17);
            CHECK(active->selectedItems == 1);
            CHECK(active->currentItem == 7);
            CHECK(active->topItem == 3);

            const auto passive = panels.panel(core::PanelSide::Passive);
            REQUIRE(passive.has_value());
            CHECK(passive->plugin);
            panelType = PTYPE_TREEPANEL;
            const auto tree = panels.panel(core::PanelSide::Passive);
            REQUIRE(tree.has_value());
            CHECK_FALSE(tree->filePanel);
            panelType = PTYPE_FILEPANEL;
            CHECK(passive->rect == core::CellRect{40, 0, 79, 24});

            CHECK(panels.directory(core::PanelSide::Active) == directoryName);
            const auto items = panels.selectedItems(core::PanelSide::Active);
            REQUIRE(items.size() == 1);
            CHECK(items[0].name == selectedName);
            CHECK(items[0].size == 9);
            CHECK(items[0].directory);
            CHECK(items[0].userData.value == 23);

            panelsWindow = true;
            CHECK(panels.currentWindowIsPanels());
            panelsWindow = false;
            CHECK_FALSE(panels.currentWindowIsPanels());
            panels.updateAndRedraw(core::PanelSide::Passive);
            CHECK(updateCalls == 1);
            CHECK(redrawCalls == 1);
        }

        TEST_CASE("Far failures become empty optional values")
        {
            PluginStartupInfo info{};
            FarPanels panels{info};
            CHECK_FALSE(panels.panel(core::PanelSide::Active).has_value());
            CHECK_FALSE(panels.directory(core::PanelSide::Active).has_value());
            CHECK(panels.selectedItems(core::PanelSide::Active).empty());
            CHECK_FALSE(panels.currentWindowIsPanels());
            panels.updateAndRedraw(core::PanelSide::Active);

            info = startup();
            FarPanels failing{info};
            panelFailure = PanelFailure::Info;
            CHECK_FALSE(failing.panel(core::PanelSide::Active).has_value());

            panelFailure = PanelFailure::DirectorySize;
            CHECK_FALSE(failing.directory(core::PanelSide::Active).has_value());
            panelFailure = PanelFailure::DirectoryFill;
            CHECK_FALSE(failing.directory(core::PanelSide::Active).has_value());
            panelFailure = PanelFailure::DirectoryName;
            CHECK_FALSE(failing.directory(core::PanelSide::Active).has_value());

            panelFailure = PanelFailure::ItemSize;
            CHECK(failing.selectedItems(core::PanelSide::Active).empty());
            panelFailure = PanelFailure::ItemFill;
            CHECK(failing.selectedItems(core::PanelSide::Active).empty());
            panelFailure = PanelFailure::ItemName;
            const auto unnamed = failing.selectedItems(core::PanelSide::Active);
            REQUIRE(unnamed.size() == 1);
            CHECK(unnamed[0].name.empty());

            panelFailure = PanelFailure::None;
            advFailure = true;
            CHECK_FALSE(failing.currentWindowIsPanels());
            advFailure = false;
        }

        TEST_CASE("host wraps synchro, messages, plugin lookup, and guarded extraction")
        {
            auto info = startup();
            synchroCalls = 0;
            messageCalls = 0;
            pluginFailure = PluginFailure::None;
            pluginGlobal.Instance = reinterpret_cast<void *>(42);
            FarHost host{info};
            host.postSynchro();
            CHECK(synchroCalls == 1);
            const std::vector<std::wstring> lines{L"line"};
            host.message(L"title", lines);
            CHECK(messageCalls == 1);

            core::Guid guid{};
            std::memcpy(guid.data(), &ownerGuid, sizeof(ownerGuid));
            const auto module = host.pluginModule(guid);
            REQUIRE(module.has_value());
            CHECK(module->path == pluginPath);
            CHECK(module->instance == 42);

            const core::PluginModule callable{.path = GETFILES_SUCCESS_PATH, .instance = 42};
            const std::vector<core::Item> items{{.name = L"far-host.txt"}};
            const auto destination = std::filesystem::temp_directory_path() / L"burlak-far-host";
            std::filesystem::remove_all(destination);
            CHECK(host.extract(17, items, callable, destination.wstring()).has_value());
            CHECK(std::filesystem::exists(destination / L"far-host.txt"));
            std::filesystem::remove_all(destination);

            PluginStartupInfo emptyInfo{};
            FarHost emptyHost{emptyInfo};
            emptyHost.postSynchro();
            emptyHost.message(L"", {});
            CHECK_FALSE(emptyHost.pluginModule(guid).has_value());

            for (const auto failure : {PluginFailure::Find, PluginFailure::Size, PluginFailure::Fill,
                                       PluginFailure::ModuleName, PluginFailure::GlobalInfo}) {
                pluginFailure = failure;
                CHECK_FALSE(host.pluginModule(guid).has_value());
            }
            pluginFailure = PluginFailure::None;
        }
    }

} // namespace burlak::adapters::far_api
