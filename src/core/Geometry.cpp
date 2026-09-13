#include "core/Geometry.hpp"

namespace burlak::core
{

    bool isItemCell(const PanelInfo &panel, Cell cell)
    {
        // Far keeps scrolling while a button is held on the frame, titles, status rows, or
        // scrollbar, so only stable item rows can start Burlak's gesture (Far source:
        // far/filelist.cpp, FileList::ProcessMouse).
        return panel.visible && panel.realNames && cell.x > panel.rect.left && cell.x < panel.rect.right &&
               cell.y >= panel.rect.top + 2 && cell.y <= panel.rect.bottom - 3;
    }

    Point toPoint(Cell cell, const CellGeometry &geometry)
    {
        return {.x = geometry.origin.x + cell.x * geometry.cellWidth,
                .y = geometry.origin.y + cell.y * geometry.cellHeight};
    }

    Cell toCell(Point point, const CellGeometry &geometry)
    {
        return {.x = (point.x - geometry.origin.x) / geometry.cellWidth,
                .y = (point.y - geometry.origin.y) / geometry.cellHeight};
    }

} // namespace burlak::core
