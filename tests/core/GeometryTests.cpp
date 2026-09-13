#include "core/Geometry.hpp"

#include <doctest/doctest.h>

namespace burlak::core
{

    TEST_SUITE("geometry")
    {
        TEST_CASE("only the item rows strictly inside a visible real-name panel arm a gesture")
        {
            const PanelInfo panel{.visible = true, .realNames = true, .plugin = false, .rect = {0, 0, 39, 24}};

            CHECK(isItemCell(panel, {1, 2}));
            CHECK(isItemCell(panel, {38, 21}));
            CHECK_FALSE(isItemCell(panel, {0, 2}));
            CHECK_FALSE(isItemCell(panel, {39, 2}));
            CHECK_FALSE(isItemCell(panel, {1, 1}));
            CHECK_FALSE(isItemCell(panel, {1, 22}));

            auto hidden = panel;
            hidden.visible = false;
            CHECK_FALSE(isItemCell(hidden, {1, 2}));

            auto virtualNames = panel;
            virtualNames.realNames = false;
            CHECK_FALSE(isItemCell(virtualNames, {1, 2}));
        }

        TEST_CASE("cell and pixel geometry round trips")
        {
            const CellGeometry geometry{.origin = {100, 50}, .cellWidth = 8, .cellHeight = 16};
            CHECK(toPoint({3, 2}, geometry) == Point{124, 82});
            CHECK(toCell({131, 97}, geometry) == Cell{3, 2});
        }
    }

} // namespace burlak::core
