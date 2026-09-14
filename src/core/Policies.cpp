#include "core/Policies.hpp"

#include "core/Geometry.hpp"

#include <limits>

namespace burlak::core
{

    namespace
    {

        [[nodiscard]] std::size_t index(PanelSide side)
        {
            return side == PanelSide::Active ? 0U : 1U;
        }

        [[nodiscard]] PanelSide other(PanelSide side)
        {
            return side == PanelSide::Active ? PanelSide::Passive : PanelSide::Active;
        }

        [[nodiscard]] bool contains(PixelRect rect, Point point)
        {
            return point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom;
        }

        // View-mode or column-layout changes that preserve this identity are intentionally below the feature's
        // plausibility bar.
        [[nodiscard]] bool samePanel(const std::optional<PanelInfo> &before, const std::optional<PanelInfo> &current)
        {
            if (!before || !current) {
                return false;
            }
            return current->visible && current->filePanel && before->plugin == current->plugin &&
                   before->filePanel == current->filePanel && before->rect == current->rect &&
                   before->handle == current->handle && before->owner == current->owner &&
                   before->currentItem == current->currentItem && before->topItem == current->topItem;
        }

        [[nodiscard]] bool sameHost(const std::optional<HostWindow> &before, const std::optional<HostWindow> &current)
        {
            return before && current && before->handle == current->handle && before->rect == current->rect;
        }

    } // namespace

    DragAction ReleasePolicy::query(Button button, bool escapePressed, bool leftDown, bool rightDown, Effect lastEffect,
                                    bool needsExtraction, bool overOwnWindow, bool peer) const
    {
        if (escapePressed) {
            return DragAction::Cancel;
        }
        const bool trackedDown = button == Button::Left ? leftDown : rightDown;
        if (trackedDown) {
            return DragAction::Continue;
        }
        if (lastEffect != Effect::None && peer) {
            return DragAction::HandToPeer;
        }
        return lastEffect != Effect::None && needsExtraction && !overOwnWindow ? DragAction::ExtractThenDrop
                                                                               : DragAction::Drop;
    }

    Effect ReleasePolicy::feedback(bool move, bool copy, bool link) const
    {
        if (move) {
            return Effect::Move;
        }
        if (copy) {
            return Effect::Copy;
        }
        return link ? Effect::Link : Effect::None;
    }

    Effect DropPolicy::effect(const DropContext &context, Point point, bool shift) const
    {
        if (!context.host || !context.geometry || !contains(context.host->rect, point)) {
            return Effect::None;
        }
        const auto &source = context.panels[index(context.source)];
        const auto &target = context.panels[index(other(context.source))];
        // Only FileList::ProcessMouse reaches Panel::ProcessMouseDrag, so neither a non-file source
        // nor destination can participate (Far source: far/filelist.cpp, FileList::ProcessMouse).
        if (!source || !source->filePanel || !target || !target->filePanel ||
            !isItemCell(*target, toCell(point, *context.geometry))) {
            return Effect::None;
        }
        return shift ? Effect::Move : Effect::Copy;
    }

    DropDecision DropPolicy::drop(const DropContext &context, Point point, bool shift) const
    {
        const auto chosen = effect(context, point, shift);
        if (chosen == Effect::None) {
            return {};
        }
        const Modifiers modifiers{.shift = chosen == Effect::Move};
        // Far arms its panel drag from the press and completes it on a buttonless event offered to
        // the other panel (Far sources: far/panel.cpp, Panel::ProcessMouseDrag; far/filepanels.cpp,
        // FilePanels::ProcessMouse; far/filelist.cpp, FileList::ProcessCopyKeys).
        return {.effect = chosen,
                .events = {MouseEvent{.at = context.press, .left = true, .mods = modifiers},
                           MouseEvent{.at = toCell(point, *context.geometry), .mods = modifiers}}};
    }

    bool DropPolicy::sameIdentity(const DropContext &before, const DropContext &current) const
    {
        const auto source = index(before.source);
        const auto destination = index(other(before.source));
        return current.panelsWindow && current.source == before.source &&
               samePanel(before.panels[source], current.panels[source]) &&
               samePanel(before.panels[destination], current.panels[destination]) &&
               sameHost(before.host, current.host) && before.sourcePaths == current.sourcePaths &&
               before.destinationDirectory && current.destinationDirectory &&
               *before.destinationDirectory == *current.destinationDirectory;
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

    bool allPathsAdvertised(std::size_t requested, std::size_t parsed)
    {
        return requested == parsed;
    }

    std::optional<std::uint32_t> runOwner(std::wstring_view name)
    {
        const auto separator = name.find(L'-');
        if (separator == 0 || separator == std::wstring_view::npos || separator + 1 == name.size()) {
            return std::nullopt;
        }

        std::uint32_t owner{};
        for (const wchar_t character : name.substr(0, separator)) {
            if (character < L'0' || character > L'9') {
                return std::nullopt;
            }
            const auto digit = static_cast<std::uint32_t>(character - L'0');
            if (owner > (std::numeric_limits<std::uint32_t>::max() - digit) / 10U) {
                return std::nullopt;
            }
            owner = owner * 10U + digit;
        }
        return owner;
    }

    bool shouldSweepRun(std::wstring_view name, std::uint32_t currentProcess, bool ownerAlive)
    {
        const auto owner = runOwner(name);
        return owner && (*owner == currentProcess || !ownerAlive);
    }

} // namespace burlak::core
