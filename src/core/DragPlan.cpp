#include "core/DragPlan.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <set>

namespace burlak::core
{

    namespace
    {

        [[nodiscard]] bool usable(const Item &item)
        {
            return !item.name.empty() && item.name != L"." && item.name != L"..";
        }

        void duplicateMessage(IFarHost &host, std::wstring_view name)
        {
            const std::array lines{L"Duplicate plugin item name: " + std::wstring{name} + L"."};
            host.message(L"Burlak", lines);
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

        std::vector<Item> items;
        bool itemsLoaded{};
        if (panel->plugin) {
            items = panels_.selectedItems(PanelSide::Active);
            itemsLoaded = true;
            if (items.size() != panel->selectedItems) {
                return std::unexpected(Error::Unavailable);
            }
            std::erase_if(items, [](const Item &item) { return !usable(item); });
            if (items.empty()) {
                return std::unexpected(Error::NoSelection);
            }
            const auto absolute = [](const Item &item) { return std::filesystem::path{item.name}.is_absolute(); };
            const auto absoluteCount = static_cast<std::size_t>(std::ranges::count_if(items, absolute));
            if (absoluteCount == items.size()) {
                std::vector<std::wstring> paths;
                paths.reserve(items.size());
                for (const auto &item : items) {
                    paths.push_back(item.name);
                }
                return Plan{.paths = std::move(paths), .extraction = std::nullopt};
            }

            if (!panel->realNames) {
                if (absoluteCount != 0) {
                    return std::unexpected(Error::Unavailable);
                }
                const auto before = [this](std::wstring_view left, std::wstring_view right) {
                    return files_.nameBefore(left, right);
                };
                std::set<std::wstring_view, decltype(before)> names{before};
                for (const auto &item : items) {
                    const auto [existing, inserted] = names.insert(item.name);
                    if (!inserted) {
                        // Windows cannot advertise two case-insensitively equal paths in one directory, while
                        // GetFilesW must receive the original names, so this selection has no faithful OLE payload.
                        duplicateMessage(host_, *existing);
                        return std::unexpected(Error::Unavailable);
                    }
                }
                const auto module = host_.pluginModule(panel->owner);
                if (!module) {
                    return std::unexpected(Error::Unavailable);
                }
                const auto location = panels_.pluginDirectory(PanelSide::Active);
                if (!location) {
                    return std::unexpected(Error::DirectoryUnavailable);
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
                return Plan{.paths = std::move(paths),
                            .extraction = ExtractionRecipe{.panel = panel->handle,
                                                           .owner = panel->owner,
                                                           .location = *location,
                                                           .items = std::move(items),
                                                           .module = *module,
                                                           .directory = *directory}};
            }
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

        if (!itemsLoaded) {
            items = panels_.selectedItems(PanelSide::Active);
            std::erase_if(items, [](const Item &item) { return !usable(item); });
            if (items.empty()) {
                return std::unexpected(Error::NoSelection);
            }
        }
        std::vector<std::wstring> paths;
        paths.reserve(items.size());
        for (const auto &item : items) {
            paths.push_back(std::filesystem::path{item.name}.is_absolute() ? item.name : *directory + item.name);
        }
        return Plan{.paths = std::move(paths), .extraction = std::nullopt};
    }

} // namespace burlak::core
