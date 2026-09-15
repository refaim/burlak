#pragma once

#include "core/Interfaces.hpp"

#include <expected>
#include <string>
#include <vector>

namespace burlak::core
{

    struct ExtractionRecipe
    {
        PanelHandle panel{};
        Guid owner{};
        // The panel handle is the plugin's own heap pointer, which a reopened archive can reuse; the location
        // (inner directory and host archive) is what proves the release still faces the archive planned here.
        PanelDirectory location;
        std::vector<Item> items;
        PluginModule module;
        std::wstring directory;
    };

    struct Plan
    {
        std::vector<std::wstring> paths;
        std::optional<ExtractionRecipe> extraction;
    };

    class DragPlan
    {
      public:
        DragPlan(IPanels &panels, IFarHost &host, IFiles &files);
        [[nodiscard]] std::expected<Plan, Error> build();

      private:
        IPanels &panels_;
        IFarHost &host_;
        IFiles &files_;
    };

} // namespace burlak::core
