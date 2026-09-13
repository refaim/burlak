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
