#include "core/Policies.hpp"

#include <doctest/doctest.h>

namespace burlak::core
{

    TEST_SUITE("policies")
    {
        TEST_CASE("release policy cancels Escape and otherwise continues or drops")
        {
            ReleasePolicy policy;
            CHECK(policy.query(Button::Left, true, true, false, Effect::Copy, false, false, true) ==
                  DragAction::Cancel);
            CHECK(policy.query(Button::Left, false, false, true, Effect::Copy, false, false, false) ==
                  DragAction::Drop);
            CHECK(policy.query(Button::Left, false, true, false, Effect::Copy, false, false, true) ==
                  DragAction::Continue);
            CHECK(policy.query(Button::Right, false, true, false, Effect::Copy, false, false, false) ==
                  DragAction::Drop);
            CHECK(policy.query(Button::Right, false, false, true, Effect::Copy, false, false, true) ==
                  DragAction::Continue);
            CHECK(policy.query(Button::Left, false, false, false, Effect::None, false, false, true) ==
                  DragAction::Drop);
        }

        TEST_CASE("release policy extracts accepted plugin payloads except over the own tool window")
        {
            ReleasePolicy policy;
            CHECK(policy.query(Button::Left, false, false, false, Effect::Copy, true, false, false) ==
                  DragAction::ExtractThenDrop);
            CHECK(policy.query(Button::Left, false, false, false, Effect::Move, true, true, false) == DragAction::Drop);
            CHECK(policy.query(Button::Left, false, false, false, Effect::Link, false, false, false) ==
                  DragAction::Drop);
            CHECK(policy.query(Button::Left, false, false, false, Effect::Copy, true, false, true) ==
                  DragAction::HandToPeer);
        }

        TEST_CASE("release feedback prefers move, then copy, then link, then none")
        {
            ReleasePolicy policy;
            CHECK(policy.feedback(true, true, true) == Effect::Move);
            CHECK(policy.feedback(false, true, true) == Effect::Copy);
            CHECK(policy.feedback(false, false, true) == Effect::Link);
            CHECK(policy.feedback(false, false, false) == Effect::None);
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
                .geometry = CellGeometry{{100, 50}, 8, 16},
                .panelsWindow = false,
                .sourcePaths = {},
                .destinationDirectory = std::nullopt};

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
                .geometry = CellGeometry{{100, 50}, 8, 16},
                .panelsWindow = false,
                .sourcePaths = {},
                .destinationDirectory = std::nullopt};

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
                .geometry = CellGeometry{{100, 50}, 8, 16},
                .panelsWindow = false,
                .sourcePaths = {},
                .destinationDirectory = std::nullopt};
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

        TEST_CASE("drop replay identity rejects every stale panel input")
        {
            const DropContext before{
                .press = {},
                .source = PanelSide::Active,
                .panels = {PanelInfo{.visible = true, .filePanel = true, .rect = {0, 0, 39, 24}, .handle = 11},
                           PanelInfo{.visible = true, .filePanel = true, .rect = {40, 0, 79, 24}, .handle = 22}},
                .host = HostWindow{1, {0, 0, 80, 25}, false},
                .geometry = std::nullopt,
                .panelsWindow = true,
                .sourcePaths = {L"C:\\source\\one.txt", L"C:\\source\\two.txt"},
                .destinationDirectory = L"D:\\target"};
            DropPolicy policy;
            CHECK(policy.sameIdentity(before, before));

            auto snapshot = before;
            auto current = before;
            SUBCASE("another Far window type")
            {
                current.panelsWindow = false;
            }
            SUBCASE("source side changed")
            {
                current.source = PanelSide::Passive;
            }
            SUBCASE("source snapshot missing")
            {
                current.panels[0].reset();
            }
            SUBCASE("original source snapshot missing")
            {
                snapshot.panels[0].reset();
            }
            SUBCASE("destination snapshot missing")
            {
                current.panels[1].reset();
            }
            SUBCASE("original destination snapshot missing")
            {
                snapshot.panels[1].reset();
            }
            SUBCASE("source hidden")
            {
                current.panels[0]->visible = false;
            }
            SUBCASE("destination hidden")
            {
                current.panels[1]->visible = false;
            }
            SUBCASE("source is not a file panel")
            {
                current.panels[0]->filePanel = false;
            }
            SUBCASE("original source was not a file panel")
            {
                snapshot.panels[0]->filePanel = false;
            }
            SUBCASE("destination is not a file panel")
            {
                current.panels[1]->filePanel = false;
            }
            SUBCASE("source handle changed")
            {
                current.panels[0]->handle = 99;
            }
            SUBCASE("destination handle changed")
            {
                current.panels[1]->handle = 99;
            }
            SUBCASE("source owner changed while its handle stayed the same")
            {
                current.panels[0]->owner[0] = std::byte{1};
            }
            SUBCASE("source changed between ordinary and plugin panel")
            {
                current.panels[0]->plugin = true;
            }
            SUBCASE("source rectangle changed")
            {
                current.panels[0]->rect.right = 38;
            }
            SUBCASE("destination rectangle changed")
            {
                current.panels[1]->rect.left = 41;
            }
            SUBCASE("source current row changed")
            {
                current.panels[0]->currentItem = 7;
            }
            SUBCASE("source top row changed")
            {
                current.panels[0]->topItem = 3;
            }
            SUBCASE("destination current row changed")
            {
                current.panels[1]->currentItem = 8;
            }
            SUBCASE("destination top row changed")
            {
                current.panels[1]->topItem = 4;
            }
            SUBCASE("host handle changed")
            {
                current.host->handle = 99;
            }
            SUBCASE("host rectangle changed")
            {
                current.host->rect.right = 99;
            }
            SUBCASE("original host missing")
            {
                snapshot.host.reset();
            }
            SUBCASE("current host missing")
            {
                current.host.reset();
            }
            SUBCASE("selected source paths changed")
            {
                current.sourcePaths = {L"C:\\source\\other.txt"};
            }
            SUBCASE("destination directory changed")
            {
                current.destinationDirectory = L"E:\\other";
            }
            SUBCASE("original destination directory was unavailable")
            {
                snapshot.destinationDirectory.reset();
            }
            SUBCASE("current destination directory was unavailable")
            {
                current.destinationDirectory.reset();
            }

            CHECK_FALSE(policy.sameIdentity(snapshot, current));
        }

        TEST_CASE("every requested path must be represented by the shell payload")
        {
            CHECK(allPathsAdvertised(2, 2));
            CHECK_FALSE(allPathsAdvertised(2, 1));
        }

        TEST_CASE("sweep policy removes this process and dead owners but ignores live and unrelated directories")
        {
            CHECK(shouldSweepRun(L"42-1", 42, true));
            CHECK(shouldSweepRun(L"17-old", 42, false));
            CHECK_FALSE(shouldSweepRun(L"17-old", 42, true));
            CHECK_FALSE(shouldSweepRun(L"other", 42, false));
            CHECK_FALSE(shouldSweepRun(L"17", 42, false));
            CHECK_FALSE(shouldSweepRun(L"17-", 42, false));
            CHECK_FALSE(shouldSweepRun(L"-1", 42, false));
            CHECK_FALSE(shouldSweepRun(L"/-1", 42, false));
            CHECK_FALSE(shouldSweepRun(L"x-1", 42, false));
            CHECK_FALSE(shouldSweepRun(L"42949672960-1", 42, false));
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
