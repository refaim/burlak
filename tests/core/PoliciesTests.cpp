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

        TEST_CASE("drop policy accepts item rows only on the visible panel opposite the source")
        {
            DropPolicy policy;
            DropContext context{
                .press = {5, 5},
                .source = PanelSide::Active,
                .panels = {PanelInfo{.visible = true, .realNames = true, .filePanel = true, .rect = {0, 0, 39, 24}},
                           PanelInfo{.visible = true, .plugin = true, .filePanel = true, .rect = {40, 0, 79, 24}}},
                .host = HostWindow{1, {100, 50, 740, 450}, false},
                .geometry = CellGeometry{{100, 50}, 8, 16}};

            CHECK(policy.effect(context, {435, 401}, false) == Effect::Copy); // cell (41, 21), last item row
            CHECK(policy.effect(context, {435, 402}, true) == Effect::None);  // cell (41, 22), first frame row
            CHECK(policy.effect(context, {419, 130}, false) == Effect::None); // cell (39, 5), left divider frame
            CHECK(policy.effect(context, {427, 130}, false) == Effect::None); // cell (40, 5), right divider frame
            CHECK(policy.effect(context, {143, 130}, false) == Effect::None); // source panel

            context.panels[1]->visible = false;
            CHECK(policy.effect(context, {435, 130}, false) == Effect::None);
            context.panels[1]->visible = true;
            CHECK(policy.effect(context, {435, 130}, false) == Effect::Copy);
            CHECK(policy.effect(context, {435, 130}, true) == Effect::Move);

            context.panels[1]->filePanel = false;
            CHECK(policy.effect(context, {435, 130}, false) == Effect::None);
            context.panels[1]->filePanel = true;
            context.panels[0]->filePanel = false;
            CHECK(policy.effect(context, {435, 130}, false) == Effect::None);
            context.panels[0] = std::nullopt;
            CHECK(policy.effect(context, {435, 130}, false) == Effect::None);
        }

        TEST_CASE("drop policy rejects points outside the host and unavailable geometry")
        {
            DropPolicy policy;
            DropContext context{
                .press = {45, 5},
                .source = PanelSide::Passive,
                .panels = {PanelInfo{.visible = true, .realNames = true, .filePanel = true, .rect = {0, 0, 39, 24}},
                           PanelInfo{.visible = true, .realNames = true, .filePanel = true, .rect = {40, 0, 79, 24}}},
                .host = HostWindow{1, {100, 50, 740, 450}, false},
                .geometry = CellGeometry{{100, 50}, 8, 16}};

            CHECK(policy.effect(context, {99, 130}, false) == Effect::None);
            CHECK(policy.effect(context, {740, 130}, false) == Effect::None);
            CHECK(policy.effect(context, {143, 49}, false) == Effect::None);
            CHECK(policy.effect(context, {143, 450}, false) == Effect::None);
            CHECK(policy.effect(context, {143, 130}, false) == Effect::Copy);
            context.geometry.reset();
            CHECK(policy.effect(context, {143, 130}, false) == Effect::None);
            context.geometry = CellGeometry{{100, 50}, 8, 16};
            context.host.reset();
            CHECK(policy.effect(context, {143, 130}, false) == Effect::None);
            context.host = HostWindow{1, {100, 50, 740, 450}, false};
            context.panels[0].reset();
            CHECK(policy.effect(context, {143, 130}, false) == Effect::None);
        }

        TEST_CASE("drop policy builds Far's press and release records with the selected effect")
        {
            const DropContext context{
                .press = {5, 5},
                .source = PanelSide::Active,
                .panels = {PanelInfo{.visible = true, .realNames = true, .filePanel = true, .rect = {0, 0, 39, 24}},
                           PanelInfo{.visible = true, .plugin = true, .filePanel = true, .rect = {40, 0, 79, 24}}},
                .host = HostWindow{1, {100, 50, 740, 450}, false},
                .geometry = CellGeometry{{100, 50}, 8, 16}};
            DropPolicy policy;

            const auto copy = policy.drop(context, {460, 146}, false);
            CHECK(copy.effect == Effect::Copy);
            CHECK(copy.events[0] == MouseEvent{.at = {5, 5}, .left = true});
            CHECK(copy.events[1] == MouseEvent{.at = {45, 6}});

            const auto move = policy.drop(context, {460, 146}, true);
            CHECK(move.effect == Effect::Move);
            CHECK(move.events[0] == MouseEvent{.at = {5, 5}, .left = true, .mods = {.shift = true}});
            CHECK(move.events[1] == MouseEvent{.at = {45, 6}, .mods = {.shift = true}});

            const auto rejected = policy.drop(context, {140, 130}, false);
            CHECK(rejected.effect == Effect::None);
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
