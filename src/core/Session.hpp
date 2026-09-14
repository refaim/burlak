#pragma once

#include "core/DragPlan.hpp"
#include "core/Interfaces.hpp"
#include "core/Policies.hpp"

#include <deque>
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
        [[nodiscard]] Effect effect(Point point, bool shift) const override;
        [[nodiscard]] Effect drop(Point point, bool shift) override;
        [[nodiscard]] bool receivePeerDrop(PendingPeerDrop drop) override;

      private:
        struct PendingDrop
        {
            Point point{};
            Cell cell{};
            Effect effect{Effect::None};
        };

        IPanels &panels_;
        IFarHost &host_;
        IScreen &screen_;
        IInput &input_;
        IFiles &files_;
        IShell &shell_;
        DropPolicy dropPolicy_;
        PeerReceivePolicy peerDropPolicy_;
        std::optional<DropContext> farContext_;
        std::optional<DropContext> hoverContext_;
        std::mutex pendingMutex_;
        std::optional<PendingDrop> pendingDrop_;
        std::deque<PendingPeerDrop> pendingPeerDrops_;
        std::optional<Plan> plan_;
        std::optional<std::wstring> pendingExtraction_;
        std::optional<std::wstring> cleanupDirectory_;
    };

} // namespace burlak::core
