#pragma once

#include "core/Types.hpp"

#include <chrono>
#include <expected>
#include <optional>
#include <string_view>

namespace burlak::core
{

    enum class DragAction : std::uint8_t
    {
        Continue,
        Drop,
        Cancel,
        ExtractThenDrop
    };

    class IReleasePolicy
    {
      public:
        virtual ~IReleasePolicy() = default;
        [[nodiscard]] virtual DragAction query(Button button, bool escapePressed, bool leftDown, bool rightDown,
                                               Effect lastEffect, bool needsExtraction, bool overOwnWindow) const = 0;
        [[nodiscard]] virtual Effect feedback(bool move, bool copy, bool link) const = 0;
    };

    class ReleasePolicy final : public IReleasePolicy
    {
      public:
        [[nodiscard]] DragAction query(Button button, bool escapePressed, bool leftDown, bool rightDown,
                                       Effect lastEffect, bool needsExtraction, bool overOwnWindow) const override;
        [[nodiscard]] Effect feedback(bool move, bool copy, bool link) const override;
    };

    class DropPolicy final
    {
      public:
        [[nodiscard]] Effect effect(const DropContext &context, Point point, bool shift) const;
        [[nodiscard]] DropDecision drop(const DropContext &context, Point point, bool shift) const;
        [[nodiscard]] bool sameIdentity(const DropContext &before, const DropContext &current) const;
    };

    class ExternalDragPolicy final
    {
      public:
        [[nodiscard]] bool overHost(const ExternalDragFacts &facts) const;
    };

    class ReceivePolicy final
    {
      public:
        [[nodiscard]] Effect effect(const ReceiveSnapshot &snapshot, Point point, bool shift,
                                    AllowedEffects allowed = {.copy = true, .move = true}) const;
        [[nodiscard]] std::optional<ReceiveDestination> destination(const ReceiveSnapshot &snapshot, Point point) const;
        [[nodiscard]] std::optional<PixelRect> overlayRect(const ReceiveSnapshot &snapshot) const;
        [[nodiscard]] bool sameIdentity(const ReceiveSnapshot &before, const ReceiveSnapshot &current,
                                        Point point) const;
    };

    [[nodiscard]] WindowPlacement placement(bool hostTopmost);
    [[nodiscard]] bool dragCompleted(DragLoopOutcome outcome, std::int32_t droppedStatus, std::int32_t cancelledStatus);
    [[nodiscard]] std::expected<void, Error> expectedOutcome(bool succeeded, Error failure);
    [[nodiscard]] std::expected<void, Error> replayOutcome(ReplayOutcome outcome);
    [[nodiscard]] std::expected<void, Error> extractionOutcome(bool crashed, std::intptr_t result);
    [[nodiscard]] bool allPathsAdvertised(std::size_t requested, std::size_t parsed);
    [[nodiscard]] std::optional<Effect> preferredDropEffect(bool needsExtraction);
    [[nodiscard]] std::expected<int, Error> consoleRowOffset(int bufferHeight, int windowTop, int windowBottom);
    [[nodiscard]] std::optional<std::uint32_t> runOwner(std::wstring_view name);
    inline constexpr auto extractionRunGracePeriod = std::chrono::minutes{3};
    inline constexpr auto extractionSweepInterval = std::chrono::minutes{1};
    inline constexpr auto externalDragPollInterval = std::chrono::milliseconds{50};
    // The tool thread waits at most this long for Far's thread to answer the drop-time identity refresh.
    inline constexpr auto receiveDropTimeout = std::chrono::seconds{10};
    // Safety net for a source that releases the button in Entered but never sends Drop or DragLeave (crashed after
    // the release). A plugin extracting a large archive inside QueryContinueDrag can legitimately exceed ten
    // seconds, so the ceiling is two minutes rather than racing that extraction to a premature teardown; a Drop
    // that arrives after the teardown is answered NONE. The cost of waiting is real: the alpha-1 layered overlay is
    // hit-tested, so for up to two minutes after a source dies it swallows every mouse click on Far's panels
    // (the keyboard still reaches Far).
    inline constexpr auto receiveEnteredTimeout = std::chrono::minutes{2};
    [[nodiscard]] bool shouldSweepRun(bool ownRun, bool ownerAlive, bool oldEnough, bool inUse);
    [[nodiscard]] bool retainExtractedRun(bool extractionRan, DragLoopOutcome outcome, std::int32_t droppedStatus);
    [[nodiscard]] ReceiveDropOutcome receiveDropOutcome(Effect effect, bool completed);
    [[nodiscard]] Effect dropMenuEffect(DropMenuChoice choice, AllowedEffects allowed = {.copy = true, .move = true});

} // namespace burlak::core
