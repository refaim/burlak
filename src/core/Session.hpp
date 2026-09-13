#pragma once

#include "core/Interfaces.hpp"
#include "core/Policies.hpp"

#include <mutex>
#include <optional>

namespace burlak::core
{

    class Session final : public IDropSession
    {
      public:
        Session(IPanels &panels, IFarHost &host, IScreen &screen, IInput &input);
        [[nodiscard]] bool begin(IDragTool &tool, DragStart start);
        void synchro();
        void prepare(DropContext context) override;
        [[nodiscard]] Effect effect(Point point, bool shift) const override;
        [[nodiscard]] Effect drop(Point point, bool shift) override;

      private:
        struct PendingDrop
        {
            Point point{};
            Effect effect{Effect::None};
        };

        IPanels &panels_;
        IFarHost &host_;
        IScreen &screen_;
        IInput &input_;
        DropPolicy dropPolicy_;
        std::optional<DropContext> farContext_;
        std::optional<DropContext> hoverContext_;
        std::mutex pendingMutex_;
        std::optional<PendingDrop> pendingDrop_;
    };

} // namespace burlak::core
