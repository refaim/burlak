#pragma once

#include "core/Interfaces.hpp"

#include <expected>
#include <string>
#include <vector>

namespace burlak::core
{

    class DragPlan
    {
      public:
        explicit DragPlan(IPanels &panels);
        [[nodiscard]] std::expected<std::vector<std::wstring>, Error> paths();

      private:
        IPanels &panels_;
    };

} // namespace burlak::core
