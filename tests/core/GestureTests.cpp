#include "Fakes.hpp"

#include "core/Gesture.hpp"

#include <doctest/doctest.h>

namespace burlak::core
{

    using tests::mouse;

    TEST_SUITE("gesture")
    {
        TEST_CASE("left press passes, short moves are held, and the threshold releases at the press cell")
        {
            tests::Panels panels;
            panels.panels[0] = tests::visiblePanel({0, 0, 39, 24});
            tests::Host host;
            Gesture gesture{panels, host};

            auto press = mouse({5, 5}, true, false);
            CHECK(gesture.feed(press).action == VerdictAction::Pass);
            auto shortMove = mouse({7, 7}, true, false, true);
            CHECK(gesture.feed(shortMove).action == VerdictAction::Hold);
            auto threshold = mouse({8, 5}, true, false, true);
            const auto verdict = gesture.feed(threshold);
            CHECK(verdict.action == VerdictAction::Replace);
            auto release = mouse({5, 5}, false, false);
            release.rewrite = MouseEvent::Rewrite::ButtonlessRelease;
            CHECK(verdict.replacement == release);
            CHECK(host.synchros == 1);
            CHECK(gesture.synchro() == DragStart{Button::Left, {5, 5}});
            CHECK_FALSE(gesture.synchro().has_value());
        }

        TEST_CASE("right click replays its press and right drag primes Far with a left-held move")
        {
            tests::Panels panels;
            panels.panels[0] = tests::visiblePanel({0, 0, 39, 24});
            tests::Host host;
            Gesture gesture{panels, host};

            auto press = mouse({5, 5}, false, true);
            CHECK(gesture.feed(press).action == VerdictAction::Hold);
            auto release = mouse({5, 5}, false, false);
            const auto click = gesture.feed(release);
            CHECK(click.action == VerdictAction::Replace);
            CHECK(click.replacement == press);

            CHECK(gesture.feed(press).action == VerdictAction::Hold);
            auto threshold = mouse({5, 8}, false, true, true);
            const auto drag = gesture.feed(threshold);
            CHECK(drag.action == VerdictAction::Replace);
            auto leftMove = mouse({5, 5}, true, false, true);
            leftMove.rewrite = MouseEvent::Rewrite::LeftHeldMove;
            CHECK(drag.replacement == leftMove);
            CHECK(gesture.synchro() == DragStart{Button::Right, {5, 5}});
        }

        TEST_CASE("right double click stays held and a second left press rearms at its new cell")
        {
            tests::Panels panels;
            panels.panels[0] = tests::visiblePanel({0, 0, 39, 24});
            tests::Host host;
            Gesture gesture{panels, host};

            auto right = mouse({5, 5}, false, true);
            CHECK(gesture.feed(right).action == VerdictAction::Hold);
            CHECK(gesture.feed(right).action == VerdictAction::Hold);

            gesture.reset();
            auto firstLeft = mouse({5, 5}, true, false);
            auto secondLeft = mouse({6, 6}, true, false);
            CHECK(gesture.feed(firstLeft).action == VerdictAction::Pass);
            CHECK(gesture.feed(secondLeft).action == VerdictAction::Pass);
            auto threshold = mouse({9, 6}, true, false, true);
            CHECK(gesture.feed(threshold).replacement->at == Cell{6, 6});
        }

        TEST_CASE("both panels count but non-panel windows and virtual panels pass through")
        {
            tests::Panels panels;
            panels.panels[1] = tests::visiblePanel({40, 0, 79, 24});
            tests::Host host;
            Gesture gesture{panels, host};

            auto passive = mouse({45, 5}, true, false);
            CHECK(gesture.feed(passive).action == VerdictAction::Pass);
            auto move = mouse({48, 5}, true, false, true);
            CHECK(gesture.feed(move).action == VerdictAction::Replace);

            gesture.reset();
            panels.panelsWindow = false;
            auto active = mouse({45, 5}, false, true);
            CHECK(gesture.feed(active).action == VerdictAction::Pass);

            panels.panelsWindow = true;
            panels.panels[1]->realNames = false;
            CHECK(gesture.feed(active).action == VerdictAction::Pass);

            panels.panels[0] = tests::visiblePanel({0, 0, 39, 24});
            panels.panels[0]->realNames = false;
            panels.panels[1].reset();
            auto virtualActive = mouse({5, 5}, true, false);
            CHECK(gesture.feed(virtualActive).action == VerdictAction::Pass);

            panels.panels[0].reset();
            panels.panels[1] = tests::visiblePanel({40, 0, 79, 24});
            auto passiveFrame = mouse({40, 5}, true, false);
            CHECK(gesture.feed(passiveFrame).action == VerdictAction::Pass);
        }

        TEST_CASE("edge rows do not arm and idle moves and releases pass")
        {
            tests::Panels panels;
            panels.panels[0] = tests::visiblePanel({0, 0, 39, 24});
            tests::Host host;
            Gesture gesture{panels, host};

            for (const Cell cell : {Cell{1, 1}, Cell{1, 22}, Cell{0, 5}, Cell{39, 5}}) {
                auto press = mouse(cell, false, true);
                CHECK(gesture.feed(press).action == VerdictAction::Pass);
            }
            auto idleMove = mouse({5, 5}, true, false, true);
            CHECK(gesture.feed(idleMove).action == VerdictAction::Pass);
            auto idleRelease = mouse({5, 5}, false, false);
            CHECK(gesture.feed(idleRelease).action == VerdictAction::Pass);
        }

        TEST_CASE("wheel always passes and a missing tracked button resets then re-feeds")
        {
            tests::Panels panels;
            panels.panels[0] = tests::visiblePanel({0, 0, 39, 24});
            tests::Host host;
            Gesture gesture{panels, host};

            auto press = mouse({5, 5}, true, false);
            CHECK(gesture.feed(press).action == VerdictAction::Pass);
            auto wheel = mouse({5, 5}, false, false, false, true);
            CHECK(gesture.feed(wheel).action == VerdictAction::Pass);
            auto rightPress = mouse({6, 6}, false, true);
            CHECK(gesture.feed(rightPress).action == VerdictAction::Hold);

            auto threshold = mouse({9, 6}, false, true, true);
            CHECK(gesture.feed(threshold).action == VerdictAction::Replace);
            auto spentMove = mouse({10, 6}, false, true, true);
            CHECK(gesture.feed(spentMove).action == VerdictAction::Hold);
            auto release = mouse({10, 6}, false, false);
            CHECK(gesture.feed(release).action == VerdictAction::Pass);
        }

        TEST_CASE("an event observed during an active OLE drag resets the spent gesture")
        {
            tests::Panels panels;
            panels.panels[0] = tests::visiblePanel({0, 0, 39, 24});
            tests::Host host;
            Gesture gesture{panels, host};
            auto press = mouse({5, 5}, true, false);
            CHECK(gesture.feed(press).action == VerdictAction::Pass);
            auto event = mouse({6, 5}, true, false, true);
            CHECK(gesture.feed(event, true).action == VerdictAction::Pass);
            auto freshRight = mouse({5, 5}, false, true);
            CHECK(gesture.feed(freshRight).action == VerdictAction::Hold);
        }

        TEST_CASE("Far's replayed press arms and its release disarms without being swallowed")
        {
            tests::Panels panels;
            panels.panels[0] = tests::visiblePanel({0, 0, 39, 24});
            tests::Host host;
            Gesture gesture{panels, host};

            const auto press = mouse({5, 5}, true, false);
            const auto release = mouse({45, 5}, false, false);
            CHECK(gesture.feed(press).action == VerdictAction::Pass);
            CHECK(gesture.feed(release).action == VerdictAction::Pass);
            CHECK_FALSE(gesture.synchro().has_value());
        }
    }

} // namespace burlak::core
