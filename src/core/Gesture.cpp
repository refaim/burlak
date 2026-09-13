#include "core/Gesture.hpp"

#include "core/Geometry.hpp"

#include <cstdlib>

namespace burlak::core
{

    namespace
    {

        constexpr int dragThresholdCells = 3;

        [[nodiscard]] bool held(Button button, const MouseEvent &event)
        {
            return button == Button::Left ? event.left : event.right;
        }

    } // namespace

    Gesture::Gesture(IPanels &panels, IFarHost &host) : panels_{panels}, host_{host}
    {
    }

    Verdict Gesture::feed(const MouseEvent &event, bool dragActive)
    {
        if (dragActive) {
            reset();
            return {};
        }
        if (event.wheel) {
            return {};
        }

        if (phase_ == Phase::Idle) {
            if (event.moved || (!event.left && !event.right)) {
                return {};
            }
            return arm(event.left ? Button::Left : Button::Right, event);
        }

        if (!held(button_, event)) {
            // A held-back right press is still a normal click. Every other release starts over, so
            // Far sees it exactly as if Burlak had not tracked the preceding gesture (Far source:
            // far/filelist.cpp, FileList::ProcessMouse).
            const bool rightClick = phase_ == Phase::Armed && button_ == Button::Right;
            phase_ = Phase::Idle;
            if (rightClick) {
                return {.action = VerdictAction::Replace, .replacement = press_};
            }
            return feed(event, false);
        }

        if (!event.moved) {
            return button_ == Button::Left ? arm(Button::Left, event)
                                           : Verdict{.action = VerdictAction::Hold, .replacement = {}};
        }

        if (phase_ != Phase::Armed) {
            return {.action = VerdictAction::Hold, .replacement = {}};
        }

        const int dx = std::abs(event.at.x - press_.at.x);
        const int dy = std::abs(event.at.y - press_.at.y);
        if (dx < dragThresholdCells && dy < dragThresholdCells) {
            return {.action = VerdictAction::Hold, .replacement = {}};
        }

        auto replacement = press_;
        if (button_ == Button::Left) {
            // Far saw the press as its own panel drag. Releasing at that original cell ends Far's
            // state without turning a release over the other panel into a native copy (Far source:
            // far/panel.cpp, Panel::ProcessMouseDrag).
            replacement.left = false;
            replacement.right = false;
            replacement.moved = false;
            replacement.rewrite = MouseEvent::Rewrite::ButtonlessRelease;
        } else {
            // Far did not see the right press. A left-held move positions the panel cursor without
            // toggling selection or entering Far's button-only scrollbar and disk-menu paths (Far
            // source: far/filelist.cpp, FileList::ProcessMouse).
            replacement.left = true;
            replacement.right = false;
            replacement.moved = true;
            replacement.rewrite = MouseEvent::Rewrite::LeftHeldMove;
        }
        phase_ = Phase::Starting;
        host_.postSynchro();
        return {.action = VerdictAction::Replace, .replacement = replacement};
    }

    std::optional<DragStart> Gesture::synchro()
    {
        if (phase_ != Phase::Starting) {
            return std::nullopt;
        }
        phase_ = Phase::Spent;
        return DragStart{button_, press_.at};
    }

    void Gesture::reset()
    {
        // OLE consumes the physical release, so a completed drag cannot leave the gesture waiting for it.
        phase_ = Phase::Idle;
    }

    Verdict Gesture::arm(Button button, const MouseEvent &event)
    {
        if (!onPanel(event.at)) {
            phase_ = Phase::Idle;
            return {};
        }
        phase_ = Phase::Armed;
        button_ = button;
        press_ = event;
        return {.action = button == Button::Left ? VerdictAction::Pass : VerdictAction::Hold, .replacement = {}};
    }

    bool Gesture::onPanel(Cell cell)
    {
        // Only panel windows give a press item-selection meaning; both panels count because Far makes
        // the passive panel active before Burlak reads its selection (Far source: far/filelist.cpp,
        // FileList::ProcessMouse).
        if (!panels_.currentWindowIsPanels()) {
            return false;
        }
        const auto active = panels_.panel(PanelSide::Active);
        const auto passive = panels_.panel(PanelSide::Passive);
        return (active && active->realNames && isItemCell(*active, cell)) ||
               (passive && passive->realNames && isItemCell(*passive, cell));
    }

} // namespace burlak::core
