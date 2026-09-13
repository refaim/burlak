#include "core/Session.hpp"

#include "core/DragPlan.hpp"

#include <array>
#include <utility>

namespace burlak::core
{

    namespace
    {

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
        if (!panels_.currentWindowIsPanels()) {
            return false;
        }
        const auto paths = DragPlan{panels_}.paths();
        if (!paths || !tool.start()) {
            return false;
        }

        if (!screen_.buttonDown(start.button)) {
            return false;
        }
        // The panel that handled the press is active by the time Far invokes the input export
        // (Far source: far/filelist.cpp, FileList::ProcessMouse).
        // Far reports the current item as its one selected item when there is no explicit selection
        // (Far source: far/filelist.cpp, FileList::GetSelCount and FileList::PluginGetSelectedPanelItem).
        DropContext context{.press = start.press,
                            .source = PanelSide::Active,
                            .panels = {panels_.panel(PanelSide::Active), panels_.panel(PanelSide::Passive)},
                            .host = screen_.hostWindow(),
                            .geometry = std::nullopt,
                            .panelsWindow = true,
                            .sourcePaths = *paths,
                            .destinationDirectory = panels_.directory(PanelSide::Passive)};
        if (const auto geometry = screen_.cellGeometry()) {
            context.geometry = *geometry;
        }
        // Namespace lookup is slow, so 1.2.0 checked the physical button before paying that cost and
        // again immediately before synthesizing the release.
        // The synchronous prepare message copies the complete hover and identity snapshot; the tool
        // thread reads that value only after this Far-thread call and before showAndArm.
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
        hoverContext_ = std::move(context);
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
        const auto decision = dropPolicy_.drop(*hoverContext_, point, shift);
        if (decision.effect == Effect::None) {
            return Effect::None;
        }
        {
            const std::lock_guard lock{pendingMutex_};
            pendingDrop_ = PendingDrop{point, decision.events[1].at, decision.effect};
        }
        host_.postSynchro();
        return decision.effect;
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

        const auto panelsWindow = panels_.currentWindowIsPanels();
        const std::array panels{panels_.panel(PanelSide::Active), panels_.panel(PanelSide::Passive)};
        const auto paths = DragPlan{panels_}.paths();
        const auto destinationDirectory = panels_.directory(PanelSide::Passive);
        const auto host = screen_.hostWindowAt(pending->point);
        const auto geometryResult = screen_.cellGeometryAt(pending->point);

        // OLE owns ordinary keyboard input during its drag loop, but a macro or timer can navigate and a console
        // resize can remap rows (Far sources: far/filelist.cpp, FileList::MoveToMouse; far/filepanels.cpp,
        // FilePanels::SwapPanels).
        if (!farContext_) {
            report(host_, L"Panels changed during the drag; the drop was cancelled.");
            return;
        }

        DropContext fresh{.press = farContext_->press,
                          .source = farContext_->source,
                          .panels = panels,
                          .host = host,
                          .geometry = std::nullopt,
                          .panelsWindow = panelsWindow,
                          .sourcePaths = {},
                          .destinationDirectory = destinationDirectory};
        if (geometryResult) {
            fresh.geometry = *geometryResult;
        }
        if (paths) {
            fresh.sourcePaths = *paths;
        }
        if (!dropPolicy_.sameIdentity(*farContext_, fresh)) {
            report(host_, L"Panels changed during the drag; the drop was cancelled.");
            return;
        }
        const auto decision = dropPolicy_.drop(fresh, pending->point, pending->effect == Effect::Move);
        // MoveToMouse consumes the freshly mapped row, so accepting a different cell could change the target
        // directory (Far source: far/filelist.cpp, FileList::MoveToMouse).
        if (decision.effect != pending->effect || decision.events[1].at != pending->cell) {
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
