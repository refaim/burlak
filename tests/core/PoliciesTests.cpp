#include "core/Policies.hpp"

#include <doctest/doctest.h>

#include <chrono>

namespace burlak::core
{

    TEST_SUITE("policies")
    {
        TEST_CASE("release policy cancels Escape and otherwise continues or drops")
        {
            ReleasePolicy policy;
            CHECK(policy.query(Button::Left, true, true, false, Effect::Copy, false, false) == DragAction::Cancel);
            CHECK(policy.query(Button::Left, false, false, true, Effect::Copy, false, false) == DragAction::Drop);
            CHECK(policy.query(Button::Left, false, true, false, Effect::Copy, false, false) == DragAction::Continue);
            CHECK(policy.query(Button::Right, false, true, false, Effect::Copy, false, false) == DragAction::Drop);
            CHECK(policy.query(Button::Right, false, false, true, Effect::Copy, false, false) == DragAction::Continue);
            CHECK(policy.query(Button::Left, false, false, false, Effect::None, false, false) == DragAction::Drop);
        }

        TEST_CASE("release policy extracts accepted plugin payloads except over the own tool window")
        {
            ReleasePolicy policy;
            CHECK(policy.query(Button::Left, false, false, false, Effect::Copy, true, false) ==
                  DragAction::ExtractThenDrop);
            CHECK(policy.query(Button::Left, false, false, false, Effect::Move, true, true) == DragAction::Drop);
            CHECK(policy.query(Button::Left, false, false, false, Effect::Link, false, false) == DragAction::Drop);
        }

        TEST_CASE("external drags are accepted only from outside this host by its selected Far")
        {
            ExternalDragPolicy policy;
            const ExternalDragFacts candidate{.buttonDown = true,
                                              .pressRoot = 30,
                                              .pointRoot = 10,
                                              .host = 10,
                                              .console = 11,
                                              .tool = 20,
                                              .receiver = 7,
                                              .receiverAlive = true,
                                              .process = 7};
            CHECK(policy.overHost(candidate));

            auto facts = candidate;
            SUBCASE("press began in the host")
            {
                facts.pressRoot = 10;
            }
            SUBCASE("press began in the tool window")
            {
                facts.pressRoot = 20;
            }
            SUBCASE("button is up")
            {
                facts.buttonDown = false;
            }
            SUBCASE("press root is unavailable")
            {
                facts.pressRoot = 0;
            }
            SUBCASE("cursor is outside")
            {
                facts.host = 0;
            }
            SUBCASE("another root overlaps the host")
            {
                facts.pointRoot = 40;
            }
            SUBCASE("another Far most recently had focus")
            {
                facts.receiver = 8;
            }
            SUBCASE("our own drag is active")
            {
                facts.ownDragActive = true;
            }
            CHECK_FALSE(policy.overHost(facts));

            facts = candidate;
            facts.receiver.reset();
            CHECK(policy.overHost(facts));
            facts.receiver = 8;
            facts.receiverAlive = false;
            CHECK(policy.overHost(facts));
            facts.pointRoot = facts.console;
            CHECK(policy.overHost(facts));
        }

        TEST_CASE("receive policy covers only real-name file-panel item rows")
        {
            ReceiveSnapshot snapshot{
                .panelsWindow = true,
                .panels = {PanelInfo{.visible = true, .realNames = true, .filePanel = true, .rect = {0, 0, 39, 24}},
                           PanelInfo{.visible = true, .realNames = true, .filePanel = true, .rect = {40, 0, 79, 24}}},
                .directories = {L"C:\\left", L"D:\\right"},
                .host = HostWindow{10, {100, 50, 740, 450}, false},
                .geometry = CellGeometry{{100, 50}, 8, 16}};
            ReceivePolicy policy;

            CHECK(policy.effect(snapshot, {108, 82}, false) == Effect::Copy);  // first item row
            CHECK(policy.effect(snapshot, {108, 401}, true) == Effect::Move);  // last item row
            CHECK(policy.effect(snapshot, {100, 130}, false) == Effect::None); // left frame
            CHECK(policy.effect(snapshot, {420, 130}, false) == Effect::None); // divider
            CHECK(policy.effect(snapshot, {108, 66}, false) == Effect::None);  // title row
            CHECK(policy.effect(snapshot, {108, 402}, false) == Effect::None); // status row
            CHECK(policy.effect(snapshot, {108, 434}, false) == Effect::None); // command line / key bar
            CHECK(policy.effect(snapshot, {99, 130}, false) == Effect::None);  // outside the host

            const auto left = policy.destination(snapshot, {108, 130});
            REQUIRE(left.has_value());
            CHECK(left->side == PanelSide::Active);
            CHECK(left->directory == L"C:\\left");

            snapshot.panels[0]->visible = false;
            CHECK(policy.effect(snapshot, {108, 130}, false) == Effect::None);
            snapshot.panels[0]->visible = true;
            snapshot.panels[0]->realNames = false;
            CHECK(policy.effect(snapshot, {108, 130}, false) == Effect::None);
            snapshot.panels[0]->realNames = true;
            snapshot.panels[0]->filePanel = false;
            CHECK(policy.effect(snapshot, {108, 130}, false) == Effect::None);
            snapshot.panels[0]->filePanel = true;
            snapshot.panelsWindow = false;
            CHECK(policy.effect(snapshot, {108, 130}, false) == Effect::None);
            snapshot.panelsWindow = true;
            snapshot.geometry.reset();
            CHECK(policy.effect(snapshot, {108, 130}, false) == Effect::None);
            snapshot.geometry = CellGeometry{{100, 50}, 8, 16};
            snapshot.host.reset();
            CHECK(policy.effect(snapshot, {108, 130}, false) == Effect::None);
            snapshot.host = HostWindow{10, {100, 50, 740, 450}, false};
            snapshot.panels[0].reset();
            CHECK(policy.effect(snapshot, {108, 130}, false) == Effect::None);
            snapshot.panels[0] =
                PanelInfo{.visible = true, .realNames = true, .filePanel = true, .rect = {0, 0, 39, 24}};
            snapshot.directories[0].reset();
            CHECK(policy.effect(snapshot, {108, 130}, false) == Effect::None);
        }

        TEST_CASE("receive policy honours the source's allowed effect mask")
        {
            const ReceiveSnapshot snapshot{
                .panelsWindow = true,
                .panels = {PanelInfo{.visible = true, .realNames = true, .filePanel = true, .rect = {0, 0, 39, 24}},
                           std::nullopt},
                .directories = {L"C:\\left", std::nullopt},
                .host = HostWindow{10, {100, 50, 740, 450}, false},
                .geometry = CellGeometry{{100, 50}, 8, 16}};
            ReceivePolicy policy;

            CHECK(policy.effect(snapshot, {108, 82}, true, AllowedEffects{.copy = true}) == Effect::Copy);
            CHECK(policy.effect(snapshot, {108, 82}, false, AllowedEffects{.move = true}) == Effect::Move);
            CHECK(policy.effect(snapshot, {108, 82}, true, {}) == Effect::None);
            CHECK(dropMenuEffect(DropMenuChoice::Copy, AllowedEffects{.move = true}) == Effect::None);
            CHECK(dropMenuEffect(DropMenuChoice::Move, AllowedEffects{.copy = true}) == Effect::None);
            CHECK(dropMenuEffect(DropMenuChoice::Copy, AllowedEffects{.copy = true}) == Effect::Copy);
            CHECK(dropMenuEffect(DropMenuChoice::Move, AllowedEffects{.move = true}) == Effect::Move);
        }

        TEST_CASE("receive overlay is the union of qualifying panel rectangles")
        {
            ReceiveSnapshot snapshot{
                .panelsWindow = true,
                .panels = {PanelInfo{.visible = true, .realNames = true, .filePanel = true, .rect = {0, 0, 39, 24}},
                           PanelInfo{.visible = true, .realNames = true, .filePanel = true, .rect = {40, 0, 79, 24}}},
                .directories = {L"C:\\left", L"D:\\right"},
                .host = HostWindow{10, {100, 50, 740, 450}, false},
                .geometry = CellGeometry{{100, 50}, 8, 16}};
            ReceivePolicy policy;
            CHECK(policy.overlayRect(snapshot) == PixelRect{100, 50, 740, 450});

            snapshot.panels[1]->realNames = false;
            CHECK(policy.overlayRect(snapshot) == PixelRect{100, 50, 420, 450});
            snapshot.panels[0]->visible = false;
            CHECK_FALSE(policy.overlayRect(snapshot).has_value());

            snapshot = {};
            CHECK_FALSE(policy.overlayRect(snapshot).has_value());
            snapshot.panelsWindow = true;
            CHECK_FALSE(policy.overlayRect(snapshot).has_value());
            snapshot.host = HostWindow{10, {100, 50, 740, 450}, false};
            CHECK_FALSE(policy.overlayRect(snapshot).has_value());

            snapshot.geometry = CellGeometry{{100, 50}, 8, 16};
            snapshot.panels[0] =
                PanelInfo{.visible = true, .realNames = true, .filePanel = false, .rect = {0, 0, 39, 24}};
            snapshot.directories[0] = L"C:\\left";
            CHECK_FALSE(policy.overlayRect(snapshot).has_value());
            snapshot.panels[0]->filePanel = true;
            snapshot.directories[0].reset();
            CHECK_FALSE(policy.overlayRect(snapshot).has_value());
            snapshot.panels[0].reset();
            CHECK_FALSE(policy.overlayRect(snapshot).has_value());
        }

        TEST_CASE("drop-time identity follows the destination panel, its directory, the host, geometry, and window")
        {
            const ReceiveSnapshot before{.panelsWindow = true,
                                         .panels = {PanelInfo{.visible = true,
                                                              .realNames = true,
                                                              .filePanel = true,
                                                              .rect = {0, 0, 39, 24},
                                                              .handle = 11,
                                                              .selectedItems = 2,
                                                              .currentItem = 1,
                                                              .topItem = 0},
                                                    PanelInfo{.visible = true,
                                                              .realNames = true,
                                                              .filePanel = true,
                                                              .rect = {40, 0, 79, 24},
                                                              .handle = 22,
                                                              .selectedItems = 0,
                                                              .currentItem = 3,
                                                              .topItem = 0}},
                                         .directories = {L"C:\\left", L"D:\\right"},
                                         .host = HostWindow{10, {100, 50, 740, 450}, false},
                                         .geometry = CellGeometry{{100, 50}, 8, 16}};
            ReceivePolicy policy;
            const Point right{460, 130}; // cell (45, 5): an item row of the right panel
            CHECK(policy.sameIdentity(before, before, right));
            CHECK_FALSE(policy.sameIdentity(before, before, {100, 130})); // left frame: never a destination

            auto current = before;
            // A background refresh of the panel the user is not dropping into must not cancel the drop.
            current.panels[0]->currentItem = 7;
            current.panels[0]->topItem = 3;
            current.panels[0]->selectedItems = 9;
            current.panels[0]->visible = false;
            current.directories[0] = L"E:\\elsewhere";
            CHECK(policy.sameIdentity(before, current, right));
            // The drop lands in the destination directory, not on a row, so its cursor may move as well.
            current = before;
            current.panels[1]->currentItem = 8;
            current.panels[1]->topItem = 2;
            current.panels[1]->selectedItems = 4;
            CHECK(policy.sameIdentity(before, current, right));

            current = before;
            SUBCASE("destination directory changed")
            {
                current.directories[1] = L"D:\\changed";
            }
            SUBCASE("destination directory unavailable")
            {
                current.directories[1].reset();
            }
            SUBCASE("destination hidden")
            {
                current.panels[1]->visible = false;
            }
            SUBCASE("destination is no longer a file panel")
            {
                current.panels[1]->filePanel = false;
            }
            SUBCASE("destination lost its real names")
            {
                current.panels[1]->realNames = false;
            }
            SUBCASE("destination rectangle changed under the same point")
            {
                current.panels[1]->rect.left = 41;
            }
            SUBCASE("destination rectangle no longer covers the point")
            {
                current.panels[1]->rect.left = 46;
            }
            SUBCASE("destination handle changed")
            {
                current.panels[1]->handle = 99;
            }
            SUBCASE("destination owner changed")
            {
                current.panels[1]->owner[0] = std::byte{1};
            }
            SUBCASE("destination panel missing")
            {
                current.panels[1].reset();
            }
            SUBCASE("the other panel grew over the point")
            {
                current.panels[0]->rect.right = 60;
            }
            SUBCASE("panels window no longer current")
            {
                current.panelsWindow = false;
            }
            SUBCASE("host changed")
            {
                current.host->handle = 99;
            }
            SUBCASE("host missing")
            {
                current.host.reset();
            }
            SUBCASE("geometry shifted while the point still maps into the destination")
            {
                current.geometry->origin.x = 101; // cell (44, 5)
            }
            SUBCASE("geometry missing")
            {
                current.geometry.reset();
            }
            CHECK_FALSE(policy.sameIdentity(before, current, right));
        }

        TEST_CASE("receive outcomes implement optimized move semantics")
        {
            CHECK(receiveDropOutcome(Effect::Copy, true) == ReceiveDropOutcome{Effect::Copy, false});
            CHECK(receiveDropOutcome(Effect::Move, true) == ReceiveDropOutcome{Effect::None, true});
            CHECK(receiveDropOutcome(Effect::Copy, false) == ReceiveDropOutcome{});
            CHECK(receiveDropOutcome(Effect::None, true) == ReceiveDropOutcome{});
            CHECK(dropMenuEffect(DropMenuChoice::Copy) == Effect::Copy);
            CHECK(dropMenuEffect(DropMenuChoice::Move) == Effect::Move);
            CHECK(dropMenuEffect(DropMenuChoice::Cancel) == Effect::None);
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

        TEST_CASE("only placeholder-backed drags prefer copy")
        {
            CHECK(preferredDropEffect(true) == std::optional{Effect::Copy});
            CHECK_FALSE(preferredDropEffect(false).has_value());
        }

        TEST_CASE("console replay offsets window rows into the console buffer")
        {
            CHECK(consoleRowOffset(50, 0, 49) == 0);
            CHECK(consoleRowOffset(9001, 0, 49) == 8951);
            CHECK(consoleRowOffset(0, 0, 49) == std::unexpected(Error::Unavailable));
            CHECK(consoleRowOffset(-1, 0, 49) == std::unexpected(Error::Unavailable));
            CHECK(consoleRowOffset(50, 10, 9) == std::unexpected(Error::Unavailable));
            CHECK(consoleRowOffset(49, 0, 49) == std::unexpected(Error::Unavailable));
        }

        TEST_CASE("sweep policy removes old own runs and old runs whose owners are dead")
        {
            CHECK_FALSE(shouldSweepRun(true, true, false, false));
            CHECK_FALSE(shouldSweepRun(true, false, false, false));
            CHECK(shouldSweepRun(true, true, true, false));
            CHECK(shouldSweepRun(true, false, true, false));
            CHECK_FALSE(shouldSweepRun(false, true, false, false));
            CHECK_FALSE(shouldSweepRun(false, false, false, false));
            CHECK_FALSE(shouldSweepRun(false, true, true, false));
            CHECK(shouldSweepRun(false, false, true, false));
            CHECK_FALSE(shouldSweepRun(true, true, true, true));
            CHECK_FALSE(shouldSweepRun(false, false, true, true));
            CHECK(extractionRunGracePeriod == std::chrono::minutes{3});
            CHECK(extractionSweepInterval == std::chrono::minutes{1});
            CHECK(externalDragPollInterval == std::chrono::milliseconds{50});
            CHECK(receiveDropTimeout == std::chrono::seconds{10});
            CHECK(receiveEnteredTimeout == std::chrono::minutes{2});

            CHECK(runOwner(L"17-old") == 17);
            CHECK_FALSE(runOwner(L"other").has_value());
            CHECK_FALSE(runOwner(L"17").has_value());
            CHECK_FALSE(runOwner(L"17-").has_value());
            CHECK_FALSE(runOwner(L"-1").has_value());
            CHECK_FALSE(runOwner(L"/-1").has_value());
            CHECK_FALSE(runOwner(L"x-1").has_value());
            CHECK_FALSE(runOwner(L"42949672960-1").has_value());
        }

        TEST_CASE("only a completed drop with an extracted payload retains its run")
        {
            constexpr std::int32_t dropped = 0x00040100;
            constexpr std::int32_t cancelled = 0x00040101;
            constexpr std::int32_t failed = static_cast<std::int32_t>(0x80004005);
            CHECK(retainExtractedRun(true, {dropped, 1}, dropped));
            CHECK(retainExtractedRun(true, {dropped, 2}, dropped));
            CHECK_FALSE(retainExtractedRun(false, {dropped, 1}, dropped));
            CHECK_FALSE(retainExtractedRun(true, {dropped, 0}, dropped));
            CHECK_FALSE(retainExtractedRun(true, {cancelled, 1}, dropped));
            CHECK_FALSE(retainExtractedRun(true, {failed, 1}, dropped));
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
