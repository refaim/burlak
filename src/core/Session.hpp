#pragma once

#include "core/DragPlan.hpp"
#include "core/Interfaces.hpp"
#include "core/Policies.hpp"

#include <mutex>
#include <optional>

namespace burlak::core
{

    class Session final : public IDropSession, public IExtractionSession
    {
      public:
        Session(IPanels &panels, IFarHost &host, IScreen &screen, IInput &input, IFiles &files, IShell &shell);
        [[nodiscard]] bool begin(IDragTool &tool, DragStart start);
        [[nodiscard]] bool requestExtraction() override;
        void retain() override;
        std::optional<bool> synchro();
        void cleanup() override;
        void prepare(DropContext context) override;
        void endSource() override;
        [[nodiscard]] Effect effect(Point point, bool shift) const override;
        [[nodiscard]] Effect drop(Point point, bool shift) override;
        [[nodiscard]] bool requestReceiveSnapshot(Point point) override;
        [[nodiscard]] std::optional<ReceiveSnapshot> takeReceiveSnapshot();
        [[nodiscard]] bool requestReceiveRefresh(Point point) override;
        [[nodiscard]] std::optional<bool> takeReceiveRefresh();
        void prepareReceive(ReceiveSnapshot snapshot) override;
        void cancelReceive() override;
        [[nodiscard]] Effect receiveEffect(Point point, bool shift,
                                           AllowedEffects allowed = {.copy = true, .move = true}) const override;
        [[nodiscard]] NativeWindow receiveOwner() const override;
        [[nodiscard]] ReceiveDropOutcome receiveDrop(std::span<const std::wstring> paths, Point point,
                                                     Effect effect) override;

      private:
        struct PendingDrop
        {
            Point point{};
            Cell cell{};
            Effect effect{Effect::None};
        };

        struct PendingReceiveRefresh
        {
            Point point{};
            ReceiveSnapshot before;
        };

        IPanels &panels_;
        IFarHost &host_;
        IScreen &screen_;
        IInput &input_;
        IFiles &files_;
        IShell &shell_;
        DropPolicy dropPolicy_;
        ReceivePolicy receivePolicy_;
        std::optional<DropContext> farContext_;
        std::optional<DropContext> hoverContext_;
        mutable std::mutex pendingMutex_;
        std::optional<PendingDrop> pendingDrop_;
        std::optional<Point> pendingReceivePoint_;
        std::optional<ReceiveSnapshot> completedReceiveSnapshot_;
        std::optional<PendingReceiveRefresh> pendingReceiveRefresh_;
        std::optional<bool> completedReceiveRefresh_;
        std::optional<ReceiveSnapshot> receiveSnapshot_;
        std::optional<PanelSide> pendingRedraw_;
        std::optional<Plan> plan_;
        std::optional<std::wstring> pendingExtraction_;
        std::optional<std::wstring> cleanupDirectory_;
    };

} // namespace burlak::core
