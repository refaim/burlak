#pragma once

#include "core/Interfaces.hpp"

#include <algorithm>
#include <array>
#include <cwctype>
#include <optional>
#include <string>
#include <vector>

namespace burlak::tests
{

    class Panels final : public core::IPanels
    {
      public:
        std::array<std::optional<core::PanelInfo>, 2> panels{};
        std::array<std::vector<core::Item>, 2> items{};
        std::array<std::optional<std::wstring>, 2> directories{};
        bool panelsWindow{true};
        std::vector<core::PanelSide> updates;

        [[nodiscard]] std::optional<core::PanelInfo> panel(core::PanelSide side) override
        {
            return panels.at(index(side));
        }

        [[nodiscard]] std::vector<core::Item> selectedItems(core::PanelSide side) override
        {
            return items.at(index(side));
        }

        [[nodiscard]] std::optional<std::wstring> directory(core::PanelSide side) override
        {
            return directories.at(index(side));
        }

        [[nodiscard]] bool currentWindowIsPanels() override
        {
            return panelsWindow;
        }

        void updateAndRedraw(core::PanelSide side) override
        {
            updates.push_back(side);
        }

      private:
        [[nodiscard]] static constexpr std::size_t index(core::PanelSide side)
        {
            return side == core::PanelSide::Active ? 0U : 1U;
        }
    };

    class Host final : public core::IFarHost
    {
      public:
        int synchros{};
        std::vector<std::vector<std::wstring>> messages;
        std::optional<core::PluginModule> module;
        std::optional<core::Guid> requestedOwner;
        std::optional<std::expected<std::wstring, core::Error>> extractionResult;
        std::vector<core::PanelHandle> extractedPanels;
        std::vector<std::vector<core::Item>> extractedItems;
        std::vector<core::PluginModule> extractedModules;
        std::vector<std::wstring> extractionDirectories;

        void postSynchro() override
        {
            ++synchros;
        }

        void message(std::wstring_view, std::span<const std::wstring> lines) override
        {
            messages.emplace_back(lines.begin(), lines.end());
        }

        [[nodiscard]] std::optional<core::PluginModule> pluginModule(const core::Guid &owner) override
        {
            requestedOwner = owner;
            return module;
        }

        [[nodiscard]] std::expected<std::wstring, core::Error> extract(core::PanelHandle panel,
                                                                       std::span<const core::Item> items,
                                                                       const core::PluginModule &plugin,
                                                                       std::wstring_view directory) override
        {
            extractedPanels.push_back(panel);
            extractedItems.emplace_back(items.begin(), items.end());
            extractedModules.push_back(plugin);
            extractionDirectories.emplace_back(directory);
            return extractionResult.value_or(std::expected<std::wstring, core::Error>{std::wstring{directory}});
        }
    };

    class Files final : public core::IFiles
    {
      public:
        std::expected<std::wstring, core::Error> directory{L"C:\\Temp\\Burlak\\7-1"};
        std::optional<std::wstring> rejectedName;
        std::vector<std::pair<std::wstring, bool>> placeholders;
        std::vector<std::wstring> removed;
        std::vector<std::wstring> touched;
        std::expected<void, core::Error> touchResult{};
        std::vector<std::vector<std::wstring>> peerPaths;
        std::vector<std::uint32_t> peerProcesses;
        std::optional<core::AdoptedPeerPaths> adoptedResult;
        int sweeps{};
        mutable std::size_t nameComparisons{};

        [[nodiscard]] std::expected<std::wstring, core::Error> runDirectory() override
        {
            return directory;
        }

        [[nodiscard]] std::expected<std::wstring, core::Error> placeholder(std::wstring_view name,
                                                                           bool isDirectory) override
        {
            placeholders.emplace_back(name, isDirectory);
            if (rejectedName && name == *rejectedName) {
                return std::unexpected(core::Error::Unavailable);
            }
            return *directory + L"\\" + std::wstring{name};
        }

        [[nodiscard]] std::expected<void, core::Error> removeTree(std::wstring_view path) override
        {
            removed.emplace_back(path);
            return {};
        }

        [[nodiscard]] std::expected<void, core::Error> touch(std::wstring_view path) override
        {
            touched.emplace_back(path);
            return touchResult;
        }

        [[nodiscard]] core::AdoptedPeerPaths adoptPeerPaths(std::span<const std::wstring> paths,
                                                            std::uint32_t sourceProcess) override
        {
            peerPaths.emplace_back(paths.begin(), paths.end());
            peerProcesses.push_back(sourceProcess);
            if (adoptedResult) {
                return std::exchange(adoptedResult, std::nullopt).value();
            }
            return {.paths = {paths.begin(), paths.end()}, .cleanupDirectory = std::nullopt};
        }

        [[nodiscard]] bool nameBefore(std::wstring_view left, std::wstring_view right) const override
        {
            ++nameComparisons;
            return std::lexicographical_compare(
                left.begin(), left.end(), right.begin(), right.end(),
                [](wchar_t first, wchar_t second) { return std::towlower(first) < std::towlower(second); });
        }

        void sweep() override
        {
            ++sweeps;
        }
    };

    class Shell final : public core::IShell
    {
      public:
        class Data final : public DragData
        {
          public:
            [[nodiscard]] std::uintptr_t nativeHandle() const override
            {
                return 0;
            }
        };

        std::expected<void, core::Error> copyResult{};
        std::vector<core::Drop> copies;
        std::vector<std::wstring> destinations;
        std::vector<core::NativeWindow> owners;

        [[nodiscard]] std::expected<PreparedDrag, core::Error> makeDataObject(std::span<const std::wstring>,
                                                                              std::optional<core::Effect>) override
        {
            return std::unexpected(core::Error::Unavailable);
        }

        [[nodiscard]] core::DragLoopOutcome runDrag(core::NativeWindow, DragData &, std::uintptr_t, bool) override
        {
            return {};
        }

        [[nodiscard]] std::expected<void, core::Error> copy(std::span<const std::wstring> paths,
                                                            std::wstring_view destination, core::Effect effect,
                                                            core::NativeWindow owner) override
        {
            copies.push_back(core::Drop{.paths = {paths.begin(), paths.end()}, .effect = effect});
            destinations.emplace_back(destination);
            owners.push_back(owner);
            return copyResult;
        }
    };

    inline core::PanelInfo visiblePanel(core::CellRect rect)
    {
        return {.visible = true, .realNames = true, .plugin = false, .filePanel = true, .rect = rect};
    }

    inline core::MouseEvent mouse(core::Cell at, bool left, bool right, bool moved = false, bool wheel = false)
    {
        return {.at = at, .left = left, .right = right, .moved = moved, .wheel = wheel, .mods = {}};
    }

} // namespace burlak::tests
