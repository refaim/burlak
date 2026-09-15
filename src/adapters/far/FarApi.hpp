#pragma once

#include "core/Interfaces.hpp"

#include <plugin.hpp>

namespace burlak::adapters::far_api
{

    class FarPanels final : public core::IPanels
    {
      public:
        explicit FarPanels(PluginStartupInfo &info);

        [[nodiscard]] std::optional<core::PanelInfo> panel(core::PanelSide side) override;
        [[nodiscard]] std::vector<core::Item> selectedItems(core::PanelSide side) override;
        [[nodiscard]] std::optional<std::wstring> directory(core::PanelSide side) override;
        [[nodiscard]] std::optional<core::PanelDirectory> pluginDirectory(core::PanelSide side) override;
        [[nodiscard]] bool currentWindowIsPanels() override;
        void updateAndRedraw(core::PanelSide side) override;

      private:
        PluginStartupInfo &info_;
    };

    class FarHost final : public core::IFarHost
    {
      public:
        explicit FarHost(PluginStartupInfo &info);

        void postSynchro() override;
        void message(std::wstring_view title, std::span<const std::wstring> lines) override;
        [[nodiscard]] std::optional<core::PluginModule> pluginModule(const core::Guid &guid) override;
        [[nodiscard]] std::expected<std::wstring, core::Error> extract(core::PanelHandle panel,
                                                                       std::span<const core::Item> items,
                                                                       const core::PluginModule &module,
                                                                       std::wstring_view destination) override;

      private:
        PluginStartupInfo &info_;
    };

    [[nodiscard]] const UUID &pluginGuid();

} // namespace burlak::adapters::far_api
