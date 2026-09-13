#pragma once

#include "core/Types.hpp"

namespace burlak::core
{

    [[nodiscard]] bool isItemCell(const PanelInfo &panel, Cell cell);
    [[nodiscard]] Point toPoint(Cell cell, const CellGeometry &geometry);
    [[nodiscard]] Cell toCell(Point point, const CellGeometry &geometry);

} // namespace burlak::core
