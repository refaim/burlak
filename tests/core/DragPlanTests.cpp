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

            const auto paths = DragPlan{panels}.paths();
            REQUIRE(paths.has_value());
            CHECK(*paths == std::vector<std::wstring>{L"C:\\work\\one.txt", L"C:\\work\\two.bin"});
        }

        TEST_CASE("an existing trailing slash is not doubled")
        {
            tests::Panels panels;
            panels.panels[0] = tests::visiblePanel({0, 0, 39, 24});
            panels.directories[0] = L"C:\\work\\";
            panels.items[0] = {{.name = L"one.txt"}};
            CHECK(*DragPlan{panels}.paths() == std::vector<std::wstring>{L"C:\\work\\one.txt"});
        }

        TEST_CASE("missing panel, virtual names, directory, and usable items are expected failures")
        {
            tests::Panels panels;
            CHECK(DragPlan{panels}.paths().error() == Error::PanelUnavailable);

            panels.panels[0] = tests::visiblePanel({0, 0, 39, 24});
            panels.panels[0]->realNames = false;
            CHECK(DragPlan{panels}.paths().error() == Error::NoRealNames);

            panels.panels[0]->realNames = true;
            CHECK(DragPlan{panels}.paths().error() == Error::DirectoryUnavailable);

            panels.directories[0] = L"C:\\work";
            panels.items[0] = {{.name = L"."}, {.name = L".."}, {.name = L""}};
            CHECK(DragPlan{panels}.paths().error() == Error::NoSelection);

            panels.directories[0] = L"";
            CHECK(DragPlan{panels}.paths().error() == Error::DirectoryUnavailable);
        }
    }

} // namespace burlak::core
