#include "core/DragPlan.hpp"

namespace burlak::core
{

    namespace
    {

        [[nodiscard]] bool usable(const Item &item)
        {
            return !item.name.empty() && item.name != L"." && item.name != L"..";
        }

    } // namespace

    DragPlan::DragPlan(IPanels &panels, IFarHost &host, IFiles &files) : panels_{panels}, host_{host}, files_{files}
    {
    }

    std::expected<Plan, Error> DragPlan::build()
    {
        const auto panel = panels_.panel(PanelSide::Active);
        if (!panel) {
            return std::unexpected(Error::PanelUnavailable);
        }

        if (!panel->realNames) {
            if (!panel->plugin) {
                return std::unexpected(Error::NoRealNames);
            }
            auto items = panels_.selectedItems(PanelSide::Active);
            std::erase_if(items, [](const Item &item) { return !usable(item); });
            if (items.empty()) {
                return std::unexpected(Error::NoSelection);
            }
            const auto module = host_.pluginModule(panel->owner);
            if (!module) {
                return std::unexpected(Error::Unavailable);
            }
            const auto directory = files_.runDirectory();
            if (!directory) {
                return std::unexpected(directory.error());
            }

            std::vector<std::wstring> paths;
            paths.reserve(items.size());
            for (const auto &item : items) {
                const auto path = files_.placeholder(item.name, item.directory);
                if (!path) {
                    static_cast<void>(files_.removeTree(*directory));
                    return std::unexpected(path.error());
                }
                paths.push_back(*path);
            }
            return Plan{
                .paths = std::move(paths),
                .extraction = ExtractionRecipe{
                    .panel = panel->handle, .items = std::move(items), .module = *module, .directory = *directory}};
        }

        auto directory = panels_.directory(PanelSide::Active);
        if (!directory || directory->empty()) {
            return std::unexpected(Error::DirectoryUnavailable);
        }
        if (directory->back() != L'\\') {
            directory->push_back(L'\\');
        }

        auto items = panels_.selectedItems(PanelSide::Active);
        std::erase_if(items, [](const Item &item) { return !usable(item); });
        if (items.empty()) {
            return std::unexpected(Error::NoSelection);
        }
        std::vector<std::wstring> paths;
        paths.reserve(items.size());
        for (const auto &item : items) {
            paths.push_back(*directory + item.name);
        }
        return Plan{.paths = std::move(paths), .extraction = std::nullopt};
    }

} // namespace burlak::core
