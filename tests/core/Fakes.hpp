#pragma once

#include "core/Interfaces.hpp"

#include <array>
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

        void updateAndRedraw(core::PanelSide) override
        {
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

        void postSynchro() override
        {
            ++synchros;
        }

        void message(std::wstring_view, std::span<const std::wstring>) override
        {
        }

        [[nodiscard]] std::optional<core::PluginModule> pluginModule(const core::Guid &) override
        {
            return std::nullopt;
        }

        [[nodiscard]] std::expected<void, core::Error> extract(core::PanelHandle, std::span<const core::Item>,
                                                               const core::PluginModule &, std::wstring_view) override
        {
            return {};
        }
    };

    inline core::PanelInfo visiblePanel(core::CellRect rect)
    {
        return {.visible = true, .realNames = true, .plugin = false, .rect = rect};
    }

    inline core::MouseEvent mouse(core::Cell at, bool left, bool right, bool moved = false, bool wheel = false)
    {
        return {.at = at, .left = left, .right = right, .moved = moved, .wheel = wheel, .mods = {}};
    }

} // namespace burlak::tests
