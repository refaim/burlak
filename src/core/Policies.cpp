#include "core/Policies.hpp"

namespace burlak::core
{

    DragAction ReleasePolicy::query(Button button, bool escapePressed, bool leftDown, bool rightDown) const
    {
        if (escapePressed) {
            return DragAction::Cancel;
        }
        const bool trackedDown = button == Button::Left ? leftDown : rightDown;
        return trackedDown ? DragAction::Continue : DragAction::Drop;
    }

    Effect ReleasePolicy::feedback(bool move, bool copy) const
    {
        if (move) {
            return Effect::Move;
        }
        return copy ? Effect::Copy : Effect::None;
    }

    Effect DropPolicy::effect(bool, bool) const
    {
        // Burlak 1.2.0 owns this target only to give OLE a window; it never accepts a drop itself.
        return Effect::None;
    }

    WindowPlacement placement(bool hostTopmost)
    {
        return {.topmost = hostTopmost, .demoteFirst = !hostTopmost};
    }

    bool dragCompleted(DragLoopOutcome outcome, std::int32_t droppedStatus, std::int32_t cancelledStatus)
    {
        return outcome.status == droppedStatus || outcome.status == cancelledStatus;
    }

    std::expected<void, Error> expectedOutcome(bool succeeded, Error failure)
    {
        const std::array<std::expected<void, Error>, 2> outcomes{std::unexpected(failure),
                                                                 std::expected<void, Error>{}};
        return outcomes[static_cast<std::size_t>(succeeded)];
    }

    std::expected<void, Error> replayOutcome(ReplayOutcome outcome)
    {
        return expectedOutcome(outcome.status != 0 && outcome.written == outcome.requested, Error::Unavailable);
    }

    std::expected<void, Error> extractionOutcome(bool crashed, std::intptr_t result)
    {
        const bool succeeded = result == 1 || result == 2;
        const std::array<std::expected<void, Error>, 3> outcomes{std::expected<void, Error>{},
                                                                 std::unexpected(Error::ForeignCallFailed),
                                                                 std::unexpected(Error::ForeignCallCrashed)};
        const std::size_t outcome = crashed ? 2U : static_cast<std::size_t>(!succeeded);
        return outcomes[outcome];
    }

} // namespace burlak::core
