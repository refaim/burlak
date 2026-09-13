#include "core/Session.hpp"

#include "core/DragPlan.hpp"

#include <array>
#include <utility>

namespace burlak::core
{

    namespace
    {

        [[nodiscard]] bool sameSource(const DropContext &context, const std::array<std::optional<PanelInfo>, 2> &panels)
        {
            const auto &now = panels[0];
            if (!now) {
                return false;
            }
            // A queued hover already proved the active source snapshot is a file panel.
            const auto &before = *context.panels[0];
            return now->filePanel && before.handle == now->handle && before.rect == now->rect;
        }

        void report(IFarHost &host, std::wstring line)
        {
            const std::array lines{std::move(line)};
            host.message(L"Burlak", lines);
        }

        [[nodiscard]] bool complete(ReplayOutcome outcome, std::size_t expected)
        {
            return replayOutcome(outcome).has_value() && outcome.written == expected;
        }

    } // namespace

    Session::Session(IPanels &panels, IFarHost &host, IScreen &screen, IInput &input)
        : panels_{panels}, host_{host}, screen_{screen}, input_{input}
    {
    }

    bool Session::begin(IDragTool &tool, DragStart start)
    {
        const auto paths = DragPlan{panels_}.paths();
        if (!paths || !tool.start()) {
            return false;
        }

        if (!screen_.buttonDown(start.button)) {
            return false;
        }
        // The panel that handled the press is active by the time Far invokes the input export
        // (Far source: far/filelist.cpp, FileList::ProcessMouse).
        DropContext context{.press = start.press,
                            .source = PanelSide::Active,
                            .panels = {panels_.panel(PanelSide::Active), panels_.panel(PanelSide::Passive)},
                            .host = screen_.hostWindow(),
                            .geometry = std::nullopt};
        if (const auto geometry = screen_.cellGeometry()) {
            context.geometry = *geometry;
        }
        // Namespace lookup is slow, so 1.2.0 checked the physical button before paying that cost and
        // again immediately before synthesizing the release.
        // The synchronous prepare message copies the press, source side, panel snapshots, and geometry;
        // the tool thread reads that value only after this Far-thread call and before showAndArm.
        if (!tool.prepare(*paths, start.button, context)) {
            return false;
        }
        // This Far-thread snapshot is stored before showAndArm; OLE cannot publish PendingDrop until
        // afterward. ToolWindow's prepare message separately transfers the cosmetic hover snapshot by value.
        farContext_ = context;
        if (!context.host) {
            tool.abort();
            return false;
        }
        if (!screen_.buttonDown(start.button)) {
            tool.abort();
            return false;
        }

        // The fresh press over the tool window must follow this release in the serial input stream.
        input_.release(start.button);
        return tool.showAndArm();
    }

    void Session::prepare(DropContext context)
    {
        hoverContext_ = context;
    }

    Effect Session::effect(Point point, bool shift) const
    {
        return hoverContext_ ? dropPolicy_.effect(*hoverContext_, point, shift) : Effect::None;
    }

    Effect Session::drop(Point point, bool shift)
    {
        if (!hoverContext_) {
            return Effect::None;
        }
        const auto chosen = dropPolicy_.effect(*hoverContext_, point, shift);
        if (chosen == Effect::None) {
            return Effect::None;
        }
        {
            const std::lock_guard lock{pendingMutex_};
            pendingDrop_ = PendingDrop{point, chosen};
        }
        host_.postSynchro();
        return chosen;
    }

    void Session::synchro()
    {
        std::optional<PendingDrop> pending;
        {
            const std::lock_guard lock{pendingMutex_};
            pending = std::exchange(pendingDrop_, std::nullopt);
        }
        if (!pending) {
            return;
        }

        const std::array panels{panels_.panel(PanelSide::Active), panels_.panel(PanelSide::Passive)};
        const auto host = screen_.hostWindow();
        const auto geometryResult = screen_.cellGeometry();

        // OLE owns ordinary keyboard input during its drag loop, but a Far macro or timer and a console
        // resize can still invalidate the snapshot (Far source: far/filepanels.cpp, FilePanels::SwapPanels).
        if (!farContext_ || !sameSource(*farContext_, panels)) {
            report(host_, L"Panels changed during the drag; the drop was cancelled.");
            return;
        }

        DropContext fresh{.press = farContext_->press,
                          .source = farContext_->source,
                          .panels = panels,
                          .host = host,
                          .geometry = std::nullopt};
        if (geometryResult) {
            fresh.geometry = *geometryResult;
        }
        const auto decision = dropPolicy_.drop(fresh, pending->point, pending->effect == Effect::Move);
        if (decision.effect != pending->effect) {
            report(host_, L"Panels changed during the drag; the drop was cancelled.");
            return;
        }

        const auto outcome = input_.replay(decision.events);
        if (complete(outcome, decision.events.size())) {
            return;
        }

        bool recoveryFailed = false;
        if (outcome.written == 1) {
            // A buttonless event clears Far's static DragX even at the press cell (Far source:
            // far/panel.cpp, Panel::ProcessMouseDrag).
            const std::array release{MouseEvent{.at = decision.events[0].at, .mods = decision.events[0].mods}};
            recoveryFailed = !complete(input_.replay(release), release.size());
        }
        report(host_, recoveryFailed ? L"Far could not receive the drop input or its recovery release."
                                     : L"Far could not receive the complete drop input.");
    }

} // namespace burlak::core
