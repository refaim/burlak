#pragma once

#include "core/Types.hpp"

#include <expected>

namespace burlak::core
{

    enum class DragAction : std::uint8_t
    {
        Continue,
        Drop,
        Cancel
    };

    class IReleasePolicy
    {
      public:
        virtual ~IReleasePolicy() = default;
        [[nodiscard]] virtual DragAction query(Button button, bool escapePressed, bool leftDown,
                                               bool rightDown) const = 0;
        [[nodiscard]] virtual Effect feedback(bool move, bool copy) const = 0;
    };

    class ReleasePolicy final : public IReleasePolicy
    {
      public:
        [[nodiscard]] DragAction query(Button button, bool escapePressed, bool leftDown, bool rightDown) const override;
        [[nodiscard]] Effect feedback(bool move, bool copy) const override;
    };

    class IDropPolicy
    {
      public:
        virtual ~IDropPolicy() = default;
        [[nodiscard]] virtual Effect effect(bool move, bool copy) const = 0;
    };

    class DropPolicy final : public IDropPolicy
    {
      public:
        [[nodiscard]] Effect effect(bool move, bool copy) const override;
    };

    [[nodiscard]] WindowPlacement placement(bool hostTopmost);
    [[nodiscard]] bool dragCompleted(DragLoopOutcome outcome, std::int32_t droppedStatus, std::int32_t cancelledStatus);
    [[nodiscard]] std::expected<void, Error> expectedOutcome(bool succeeded, Error failure);
    [[nodiscard]] std::expected<void, Error> replayOutcome(ReplayOutcome outcome);
    [[nodiscard]] std::expected<void, Error> extractionOutcome(bool crashed, std::intptr_t result);

} // namespace burlak::core
