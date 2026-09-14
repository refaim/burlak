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
        ExtractThenDrop,
        HandToPeer
    };

    class IReleasePolicy
    {
      public:
        virtual ~IReleasePolicy() = default;
        [[nodiscard]] virtual DragAction query(Button button, bool escapePressed, bool leftDown, bool rightDown,
                                               Effect lastEffect, bool needsExtraction, bool overOwnWindow,
                                               bool peer) const = 0;
        [[nodiscard]] virtual Effect feedback(bool move, bool copy, bool link) const = 0;
    };

    class ReleasePolicy final : public IReleasePolicy
    {
      public:
        [[nodiscard]] DragAction query(Button button, bool escapePressed, bool leftDown, bool rightDown,
                                       Effect lastEffect, bool needsExtraction, bool overOwnWindow,
                                       bool peer) const override;
        [[nodiscard]] Effect feedback(bool move, bool copy, bool link) const override;
    };

    class DropPolicy final
    {
      public:
        [[nodiscard]] Effect effect(const DropContext &context, Point point, bool shift) const;
        [[nodiscard]] DropDecision drop(const DropContext &context, Point point, bool shift) const;
        [[nodiscard]] bool sameIdentity(const DropContext &before, const DropContext &current) const;
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
    inline constexpr auto extractionRunGracePeriod = std::chrono::minutes{10};
    [[nodiscard]] bool shouldSweepRun(bool ownRun, bool ownerAlive, bool oldEnough);
    [[nodiscard]] bool retainExtractedRun(bool extractionRan, DragLoopOutcome outcome, std::int32_t droppedStatus);

} // namespace burlak::core
