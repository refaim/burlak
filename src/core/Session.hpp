#pragma once

#include "core/Interfaces.hpp"

namespace burlak::core
{

    class Session
    {
      public:
        Session(IPanels &panels, IScreen &screen, IInput &input, IDragTool &tool);
        [[nodiscard]] bool begin(Button button);

      private:
        IPanels &panels_;
        IScreen &screen_;
        IInput &input_;
        IDragTool &tool_;
    };

} // namespace burlak::core
