#include "Fakes.hpp"

#include "core/DragPlan.hpp"

#include <doctest/doctest.h>

namespace burlak::core
{

    TEST_SUITE("drag plan")
    {
        TEST_CASE("selected real names are joined to the active directory and dot entries are skipped")
        {
            tests::Panels panels;
            panels.panels[0] = tests::visiblePanel({0, 0, 39, 24});
            panels.directories[0] = L"C:\\work";
            panels.items[0] = {{.name = L"one.txt"}, {.name = L".."}, {.name = L"."}, {.name = L"two.bin"}};
            tests::Host host;
            tests::Files files;

            const auto plan = DragPlan{panels, host, files}.build();
            REQUIRE(plan.has_value());
            CHECK(plan->paths == std::vector<std::wstring>{L"C:\\work\\one.txt", L"C:\\work\\two.bin"});
            CHECK_FALSE(plan->extraction.has_value());
        }

        TEST_CASE("an existing trailing slash is not doubled")
        {
            tests::Panels panels;
            panels.panels[0] = tests::visiblePanel({0, 0, 39, 24});
            panels.directories[0] = L"C:\\work\\";
            panels.items[0] = {{.name = L"one.txt"}};
            tests::Host host;
            tests::Files files;
            CHECK(DragPlan{panels, host, files}.build()->paths == std::vector<std::wstring>{L"C:\\work\\one.txt"});
        }

        TEST_CASE("missing panel, virtual names, directory, and usable items are expected failures")
        {
            tests::Panels panels;
            tests::Host host;
            tests::Files files;
            CHECK(DragPlan{panels, host, files}.build().error() == Error::PanelUnavailable);

            panels.panels[0] = tests::visiblePanel({0, 0, 39, 24});
            panels.panels[0]->realNames = false;
            CHECK(DragPlan{panels, host, files}.build().error() == Error::NoRealNames);

            panels.panels[0]->realNames = true;
            CHECK(DragPlan{panels, host, files}.build().error() == Error::DirectoryUnavailable);

            panels.directories[0] = L"C:\\work";
            panels.items[0] = {{.name = L"."}, {.name = L".."}, {.name = L""}};
            CHECK(DragPlan{panels, host, files}.build().error() == Error::NoSelection);

            panels.directories[0] = L"";
            CHECK(DragPlan{panels, host, files}.build().error() == Error::DirectoryUnavailable);
        }

        TEST_CASE("plugin panels create placeholders and retain a faithful extraction recipe")
        {
            tests::Panels panels;
            panels.panels[0] = tests::visiblePanel({0, 0, 39, 24});
            panels.panels[0]->realNames = false;
            panels.panels[0]->plugin = true;
            panels.panels[0]->handle = 71;
            panels.panels[0]->owner[0] = std::byte{9};
            panels.items[0] = {
                {.name = L"one.txt", .size = 19, .attributes = 2, .selected = true, .userData = {.value = 23}},
                {.name = L"folder", .attributes = 16, .directory = true, .selected = true}};
            panels.panels[0]->selectedItems = panels.items[0].size();
            tests::Host host;
            host.module = PluginModule{.path = L"Archive.dll", .instance = 42};
            tests::Files files;

            const auto plan = DragPlan{panels, host, files}.build();
            REQUIRE(plan.has_value());
            CHECK(plan->paths ==
                  std::vector<std::wstring>{L"C:\\Temp\\Burlak\\7-1\\one.txt", L"C:\\Temp\\Burlak\\7-1\\folder"});
            REQUIRE(plan->extraction.has_value());
            CHECK(plan->extraction->panel == 71);
            CHECK(plan->extraction->items == panels.items[0]);
            CHECK(plan->extraction->module.path == L"Archive.dll");
            CHECK(plan->extraction->module.instance == 42);
            CHECK(plan->extraction->directory == L"C:\\Temp\\Burlak\\7-1");
            CHECK(files.placeholders ==
                  std::vector<std::pair<std::wstring, bool>>{{L"one.txt", false}, {L"folder", true}});
            CHECK(host.requestedOwner == panels.panels[0]->owner);
        }

        TEST_CASE("plugin plans fail without a module and clean a partially built placeholder run")
        {
            tests::Panels panels;
            panels.panels[0] = tests::visiblePanel({0, 0, 39, 24});
            panels.panels[0]->realNames = false;
            panels.panels[0]->plugin = true;
            panels.panels[0]->owner[0] = std::byte{1};
            panels.items[0] = {{.name = L"one.txt"}, {.name = L"two.txt"}};
            panels.panels[0]->selectedItems = panels.items[0].size();
            tests::Host host;
            tests::Files files;

            CHECK(DragPlan{panels, host, files}.build().error() == Error::Unavailable);
            CHECK(files.placeholders.empty());

            host.module = PluginModule{.path = L"Archive.dll", .instance = 42};
            files.directory = std::unexpected(Error::Unavailable);
            CHECK(DragPlan{panels, host, files}.build().error() == Error::Unavailable);

            files.directory = L"C:\\Temp\\Burlak\\7-1";
            files.rejectedName = L"two.txt";
            CHECK(DragPlan{panels, host, files}.build().error() == Error::Unavailable);
            CHECK(files.removed == std::vector<std::wstring>{L"C:\\Temp\\Burlak\\7-1"});

            panels.items[0] = {{.name = L"."}, {.name = L".."}, {.name = L""}};
            panels.panels[0]->selectedItems = panels.items[0].size();
            CHECK(DragPlan{panels, host, files}.build().error() == Error::NoSelection);
        }

        TEST_CASE("plugin plans reject an incomplete selected-item snapshot")
        {
            tests::Panels panels;
            panels.panels[0] = tests::visiblePanel({0, 0, 39, 24});
            panels.panels[0]->realNames = false;
            panels.panels[0]->plugin = true;
            panels.panels[0]->selectedItems = 2;
            panels.items[0] = {{.name = L"one.txt"}};
            tests::Host host;
            host.module = PluginModule{.path = L"Archive.dll", .instance = 42};
            tests::Files files;

            CHECK(DragPlan{panels, host, files}.build() == std::unexpected(Error::Unavailable));
            CHECK_FALSE(host.requestedOwner.has_value());
            CHECK(files.placeholders.empty());
        }

        TEST_CASE("plugin plans reject case-insensitive duplicate placeholder names before creating a run")
        {
            tests::Panels panels;
            panels.panels[0] = tests::visiblePanel({0, 0, 39, 24});
            panels.panels[0]->realNames = false;
            panels.panels[0]->plugin = true;
            panels.panels[0]->owner[0] = std::byte{1};
            panels.items[0] = {{.name = L"Report.txt"}, {.name = L"REPORT.TXT"}};
            panels.panels[0]->selectedItems = panels.items[0].size();
            tests::Host host;
            host.module = PluginModule{.path = L"Archive.dll", .instance = 42};
            tests::Files files;

            CHECK(DragPlan{panels, host, files}.build().error() == Error::Unavailable);
            CHECK(files.placeholders.empty());
            CHECK(files.removed.empty());
            REQUIRE(host.messages.size() == 1);
            REQUIRE(host.messages[0].size() == 1);
            CHECK(host.messages[0][0].find(L"Report.txt") != std::wstring::npos);
        }

        TEST_CASE("item identity compares every stable field but not its native buffer address")
        {
            const Item original{.identity = {std::byte{1}},
                                .native = {std::byte{2}},
                                .name = L"one.txt",
                                .size = 3,
                                .attributes = 4,
                                .directory = true,
                                .selected = true,
                                .userData = {.value = 5}};
            auto changed = original;
            changed.native = {std::byte{9}};
            CHECK(changed == original);
            changed.identity = {std::byte{9}};
            CHECK_FALSE(changed == original);
            changed = original;
            changed.name = L"two.txt";
            CHECK_FALSE(changed == original);
            changed = original;
            changed.size = 6;
            CHECK_FALSE(changed == original);
            changed = original;
            changed.attributes = 7;
            CHECK_FALSE(changed == original);
            changed = original;
            changed.directory = false;
            CHECK_FALSE(changed == original);
            changed = original;
            changed.selected = false;
            CHECK_FALSE(changed == original);
            changed = original;
            changed.userData.value = 8;
            CHECK_FALSE(changed == original);
        }
    }

} // namespace burlak::core
