#include "core/DragPlan.hpp"

namespace burlak::core
{

    DragPlan::DragPlan(IPanels &panels) : panels_{panels}
    {
    }

    std::expected<std::vector<std::wstring>, Error> DragPlan::paths()
    {
        const auto panel = panels_.panel(PanelSide::Active);
        if (!panel) {
            return std::unexpected(Error::PanelUnavailable);
        }
        if (!panel->realNames) {
            return std::unexpected(Error::NoRealNames);
        }

        auto directory = panels_.directory(PanelSide::Active);
        if (!directory || directory->empty()) {
            return std::unexpected(Error::DirectoryUnavailable);
        }
        if (directory->back() != L'\\') {
            directory->push_back(L'\\');
        }

        std::vector<std::wstring> paths;
        for (const auto &item : panels_.selectedItems(PanelSide::Active)) {
            if (item.name.empty() || item.name == L"." || item.name == L"..") {
                continue;
            }
            paths.push_back(*directory + item.name);
        }
        if (paths.empty()) {
            return std::unexpected(Error::NoSelection);
        }
        return paths;
    }

} // namespace burlak::core
