#include "core/Policies.hpp"

#include <doctest/doctest.h>

namespace burlak::core
{

    TEST_SUITE("policies")
    {
        TEST_CASE("release policy cancels escape, drops a release, and otherwise continues")
        {
            ReleasePolicy policy;
            CHECK(policy.query(Button::Left, true, true, false) == DragAction::Cancel);
            CHECK(policy.query(Button::Left, false, false, true) == DragAction::Drop);
            CHECK(policy.query(Button::Left, false, true, false) == DragAction::Continue);
            CHECK(policy.query(Button::Right, false, true, false) == DragAction::Drop);
            CHECK(policy.query(Button::Right, false, false, true) == DragAction::Continue);
        }

        TEST_CASE("release feedback prefers move, then copy, then none")
        {
            ReleasePolicy policy;
            CHECK(policy.feedback(true, true) == Effect::Move);
            CHECK(policy.feedback(false, true) == Effect::Copy);
            CHECK(policy.feedback(false, false) == Effect::None);
        }

        TEST_CASE("the 1.2 drop policy remains inert")
        {
            DropPolicy policy;
            CHECK(policy.effect(false, false) == Effect::None);
            CHECK(policy.effect(false, true) == Effect::None);
            CHECK(policy.effect(true, false) == Effect::None);
        }

        TEST_CASE("window placement follows the host and always demotes before ordinary placement")
        {
            CHECK(placement(false) == WindowPlacement{false, true});
            CHECK(placement(true) == WindowPlacement{true, false});
        }

        TEST_CASE("raw OLE outcomes are interpreted in core")
        {
            constexpr std::int32_t dropped = 0x00040100;
            constexpr std::int32_t cancelled = 0x00040101;
            CHECK(dragCompleted({dropped, 1}, dropped, cancelled));
            CHECK(dragCompleted({cancelled, 0}, dropped, cancelled));
            CHECK_FALSE(dragCompleted({static_cast<std::int32_t>(0x80004005), 0}, dropped, cancelled));
        }

        TEST_CASE("foreign success and plugin extraction results map to expected values in core")
        {
            CHECK(expectedOutcome(true, Error::Unavailable).has_value());
            CHECK(expectedOutcome(false, Error::Unavailable) == std::unexpected(Error::Unavailable));
            CHECK(replayOutcome({true, 2, 2}).has_value());
            CHECK(replayOutcome({true, 2, 1}) == std::unexpected(Error::Unavailable));
            CHECK(replayOutcome({false, 2, 0}) == std::unexpected(Error::Unavailable));
            CHECK(extractionOutcome(false, 1).has_value());
            CHECK(extractionOutcome(false, 2).has_value());
            CHECK(extractionOutcome(false, 0) == std::unexpected(Error::ForeignCallFailed));
            CHECK(extractionOutcome(true, 1) == std::unexpected(Error::ForeignCallCrashed));
        }
    }

} // namespace burlak::core
