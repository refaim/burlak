#include "adapters/far/FarApi.hpp"

#include "adapters/far/PluginCall.hpp"

#include <array>
#include <bit>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

namespace burlak::adapters::far_api
{

    namespace
    {

        const UUID burlakGuid{0x130a60a7, 0x8d79, 0x483c, {0x93, 0xf0, 0x8d, 0xca, 0xb2, 0x70, 0x22, 0xc9}};

        [[nodiscard]] HANDLE farPanel(core::PanelSide side)
        {
            // Far defines active and passive panels as intentional non-null pseudo-HANDLE constants.
            // cppcheck-suppress intToPointerCast
            return side == core::PanelSide::Active ? PANEL_ACTIVE : PANEL_PASSIVE;
        }

        template <typename Value> void appendValue(std::vector<std::byte> &identity, const Value &value)
        {
            const auto bytes = std::bit_cast<std::array<std::byte, sizeof(Value)>>(value);
            identity.insert(identity.end(), bytes.begin(), bytes.end());
        }

        void appendText(std::vector<std::byte> &identity, std::optional<std::wstring_view> text)
        {
            appendValue(identity, text.has_value());
            if (!text) {
                return;
            }
            appendValue(identity, text->size());
            for (const auto character : *text) {
                appendValue(identity, character);
            }
        }

        [[nodiscard]] std::vector<std::byte> identityOf(const PluginPanelItem &item)
        {
            std::vector<std::byte> identity;
            appendValue(identity, item.CreationTime.dwLowDateTime);
            appendValue(identity, item.CreationTime.dwHighDateTime);
            appendValue(identity, item.LastAccessTime.dwLowDateTime);
            appendValue(identity, item.LastAccessTime.dwHighDateTime);
            appendValue(identity, item.LastWriteTime.dwLowDateTime);
            appendValue(identity, item.LastWriteTime.dwHighDateTime);
            appendValue(identity, item.ChangeTime.dwLowDateTime);
            appendValue(identity, item.ChangeTime.dwHighDateTime);
            appendValue(identity, item.FileSize);
            appendValue(identity, item.AllocationSize);
            appendText(identity,
                       item.FileName == nullptr ? std::nullopt : std::optional{std::wstring_view{item.FileName}});
            appendText(identity, item.AlternateFileName == nullptr
                                     ? std::nullopt
                                     : std::optional{std::wstring_view{item.AlternateFileName}});
            appendText(identity,
                       item.Description == nullptr ? std::nullopt : std::optional{std::wstring_view{item.Description}});
            appendText(identity, item.Owner == nullptr ? std::nullopt : std::optional{std::wstring_view{item.Owner}});
            appendValue(identity, item.CustomColumnData != nullptr);
            appendValue(identity, item.CustomColumnNumber);
            if (item.CustomColumnData != nullptr) {
                for (std::size_t index = 0; index < item.CustomColumnNumber; ++index) {
                    appendText(identity, item.CustomColumnData[index] == nullptr
                                             ? std::nullopt
                                             : std::optional{std::wstring_view{item.CustomColumnData[index]}});
                }
            }
            appendValue(identity, item.Flags);
            appendValue(identity, reinterpret_cast<std::uintptr_t>(item.UserData.Data));
            appendValue(identity, item.UserData.FreeData);
            appendValue(identity, item.FileAttributes);
            appendValue(identity, item.NumberOfLinks);
            appendValue(identity, item.CRC32);
            appendValue(identity, item.Reserved[0]);
            appendValue(identity, item.Reserved[1]);
            return identity;
        }

    } // namespace

    FarPanels::FarPanels(PluginStartupInfo &info) : info_{info}
    {
    }

    std::optional<core::PanelInfo> FarPanels::panel(core::PanelSide side)
    {
        if (info_.PanelControl == nullptr) {
            return std::nullopt;
        }
        PanelInfo info{};
        info.StructSize = sizeof(info);
        if (!info_.PanelControl(farPanel(side), FCTL_GETPANELINFO, 0, &info)) {
            return std::nullopt;
        }
        core::Guid owner{};
        static_assert(sizeof(owner) == sizeof(info.OwnerGuid));
        std::memcpy(owner.data(), &info.OwnerGuid, sizeof(info.OwnerGuid));
        return core::PanelInfo{
            .visible = (info.Flags & PFLAGS_VISIBLE) != 0,
            .realNames = (info.Flags & PFLAGS_REALNAMES) != 0,
            .plugin = (info.Flags & PFLAGS_PLUGIN) != 0,
            .filePanel = info.PanelType == PTYPE_FILEPANEL,
            .rect = {info.PanelRect.left, info.PanelRect.top, info.PanelRect.right, info.PanelRect.bottom},
            .handle = reinterpret_cast<core::PanelHandle>(info.PluginHandle),
            .owner = owner,
            .selectedItems = info.SelectedItemsNumber,
            // MoveToMouse interprets a replayed row relative to these two indices (Far source:
            // far/filelist.cpp, FileList::PluginGetPanelInfo and FileList::MoveToMouse).
            .currentItem = info.CurrentItem,
            .topItem = info.TopPanelItem};
    }

    std::vector<core::Item> FarPanels::selectedItems(core::PanelSide side)
    {
        const auto details = panel(side);
        if (!details) {
            return {};
        }

        std::vector<core::Item> items;
        items.reserve(details->selectedItems);
        for (std::size_t index = 0; index < details->selectedItems; ++index) {
            const auto size =
                info_.PanelControl(farPanel(side), FCTL_GETSELECTEDPANELITEM, static_cast<intptr_t>(index), nullptr);
            if (size <= 0) {
                continue;
            }
            std::vector<std::byte> buffer(static_cast<std::size_t>(size));
            FarGetPluginPanelItem request{sizeof(request), buffer.size(),
                                          reinterpret_cast<PluginPanelItem *>(buffer.data())};
            if (!info_.PanelControl(farPanel(side), FCTL_GETSELECTEDPANELITEM, static_cast<intptr_t>(index),
                                    &request)) {
                continue;
            }
            const auto &item = *request.Item;
            core::Item mapped{.identity = identityOf(item),
                              .name = item.FileName == nullptr ? L"" : item.FileName,
                              .size = item.FileSize,
                              .attributes = item.FileAttributes,
                              .directory = (item.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                              .selected = (item.Flags & PPIF_SELECTED) != 0,
                              .userData = {.value = reinterpret_cast<std::uintptr_t>(item.UserData.Data)}};
            // CreatePluginItemList forwards Far's complete PluginPanelItem records, including their pointer-backed
            // fields, so the buffer returned by FCTL_GETSELECTEDPANELITEM must stay intact until GetFilesW
            // (Far source: far/filelist.cpp, FileList::CreatePluginItemList).
            mapped.native = std::move(buffer);
            items.push_back(std::move(mapped));
        }
        return items;
    }

    std::optional<std::wstring> FarPanels::directory(core::PanelSide side)
    {
        return pluginDirectory(side).transform([](const core::PanelDirectory &location) { return location.name; });
    }

    std::optional<core::PanelDirectory> FarPanels::pluginDirectory(core::PanelSide side)
    {
        if (info_.PanelControl == nullptr) {
            return std::nullopt;
        }
        const auto size = info_.PanelControl(farPanel(side), FCTL_GETPANELDIRECTORY, 0, nullptr);
        if (size <= 0) {
            return std::nullopt;
        }
        std::vector<std::byte> buffer(static_cast<std::size_t>(size));
        auto &directory = *reinterpret_cast<FarPanelDirectory *>(buffer.data());
        directory.StructSize = sizeof(directory);
        if (!info_.PanelControl(farPanel(side), FCTL_GETPANELDIRECTORY, size, &directory) ||
            directory.Name == nullptr) {
            return std::nullopt;
        }
        // File is the host file a plugin panel was opened from and stays null for a plain directory panel.
        return core::PanelDirectory{.name = directory.Name,
                                    .file = directory.File == nullptr ? std::wstring{} : std::wstring{directory.File}};
    }

    bool FarPanels::currentWindowIsPanels()
    {
        if (info_.AdvControl == nullptr) {
            return false;
        }
        WindowInfo window{};
        window.StructSize = sizeof(window);
        window.Pos = -1;
        return info_.AdvControl(&burlakGuid, ACTL_GETWINDOWINFO, 0, &window) != 0 && window.Type == WTYPE_PANELS;
    }

    void FarPanels::updateAndRedraw(core::PanelSide side)
    {
        if (info_.PanelControl == nullptr) {
            return;
        }
        static_cast<void>(info_.PanelControl(farPanel(side), FCTL_UPDATEPANEL, 0, nullptr));
        static_cast<void>(info_.PanelControl(farPanel(side), FCTL_REDRAWPANEL, 0, nullptr));
    }

    FarHost::FarHost(PluginStartupInfo &info) : info_{info}
    {
    }

    void FarHost::postSynchro()
    {
        if (info_.AdvControl != nullptr) {
            static_cast<void>(info_.AdvControl(&burlakGuid, ACTL_SYNCHRO, 0, nullptr));
        }
    }

    void FarHost::message(std::wstring_view title, std::span<const std::wstring> lines)
    {
        if (info_.Message == nullptr) {
            return;
        }
        const std::wstring titleText{title};
        std::vector<const wchar_t *> items;
        items.reserve(lines.size() + 1);
        items.push_back(titleText.c_str());
        for (const auto &line : lines) {
            items.push_back(line.c_str());
        }
        static_cast<void>(
            info_.Message(&burlakGuid, nullptr, FMSG_WARNING | FMSG_MB_OK, nullptr, items.data(), items.size(), 1));
    }

    std::optional<core::PluginModule> FarHost::pluginModule(const core::Guid &guid)
    {
        if (info_.PluginsControl == nullptr) {
            return std::nullopt;
        }
        UUID foreignGuid{};
        static_assert(sizeof(guid) == sizeof(foreignGuid));
        std::memcpy(&foreignGuid, guid.data(), sizeof(foreignGuid));
        const auto foundValue = info_.PluginsControl(nullptr, PCTL_FINDPLUGIN, PFM_GUID, &foreignGuid);
        if (foundValue == 0) {
            return std::nullopt;
        }
        const auto found = reinterpret_cast<HANDLE>(foundValue);
        const auto size = info_.PluginsControl(found, PCTL_GETPLUGININFORMATION, 0, nullptr);
        if (size <= 0) {
            return std::nullopt;
        }
        std::vector<std::byte> buffer(static_cast<std::size_t>(size));
        auto &information = *reinterpret_cast<FarGetPluginInformation *>(buffer.data());
        information.StructSize = sizeof(information);
        if (!info_.PluginsControl(found, PCTL_GETPLUGININFORMATION, size, &information) ||
            information.ModuleName == nullptr || information.GInfo == nullptr) {
            return std::nullopt;
        }
        core::PluginModule module{.path = information.ModuleName,
                                  .instance = reinterpret_cast<core::PluginInstance>(information.GInfo->Instance)};
        return hasGetFilesExport(module) ? std::optional{std::move(module)} : std::nullopt;
    }

    std::expected<std::wstring, core::Error> FarHost::extract(core::PanelHandle panel,
                                                              std::span<const core::Item> items,
                                                              const core::PluginModule &module,
                                                              std::wstring_view destination)
    {
        return callPluginGetFiles(panel, items, module, destination);
    }

    const UUID &pluginGuid()
    {
        return burlakGuid;
    }

} // namespace burlak::adapters::far_api
