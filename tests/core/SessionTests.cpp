#include "Fakes.hpp"

#include "core/Session.hpp"

#include <doctest/doctest.h>

namespace burlak::core
{

    namespace
    {

        class Screen final : public IScreen
        {
          public:
            explicit Screen(std::vector<std::string> &callLog) : calls{callLog}
            {
            }

            std::vector<std::string> &calls;
            std::vector<bool> buttons{true, true};
            std::optional<HostWindow> window{HostWindow{1, {0, 0, 100, 100}, false}};
            std::expected<CellGeometry, Error> geometry{CellGeometry{{0, 0}, 1, 1}};
            std::optional<HostWindow> pointWindow{window};
            std::expected<CellGeometry, Error> pointGeometry{geometry};
            std::optional<Point> queriedPoint;

            [[nodiscard]] std::optional<Point> cursor() override
            {
                return {};
            }

            [[nodiscard]] bool buttonDown(Button) override
            {
                calls.emplace_back("button");
                const bool result = buttons.front();
                buttons.erase(buttons.begin());
                return result;
            }

            [[nodiscard]] std::optional<HostWindow> hostWindow() override
            {
                calls.emplace_back("host");
                return window;
            }

            [[nodiscard]] std::expected<CellGeometry, Error> cellGeometry() override
            {
                calls.emplace_back("geometry");
                return geometry;
            }

            [[nodiscard]] std::optional<HostWindow> hostWindowAt(Point point) override
            {
                calls.emplace_back("point host");
                queriedPoint = point;
                return pointWindow;
            }

            [[nodiscard]] std::expected<CellGeometry, Error> cellGeometryAt(Point point) override
            {
                calls.emplace_back("point geometry");
                queriedPoint = point;
                return pointGeometry;
            }
        };

        class Input final : public IInput
        {
          public:
            explicit Input(std::vector<std::string> &callLog) : calls{callLog}
            {
            }

            std::vector<std::string> &calls;
            std::vector<std::vector<MouseEvent>> replays;
            std::vector<ReplayOutcome> outcomes;

            void release(Button) override
            {
                calls.emplace_back("release");
            }

            void press(Button) override
            {
            }
            [[nodiscard]] ReplayOutcome replay(std::span<const MouseEvent> events) override
            {
                replays.emplace_back(events.begin(), events.end());
                if (!outcomes.empty()) {
                    const auto outcome = outcomes.front();
                    outcomes.erase(outcomes.begin());
                    return outcome;
                }
                const auto count = static_cast<std::uint32_t>(events.size());
                return {true, count, count};
            }
        };

        class Tool final : public IDragTool
        {
          public:
            explicit Tool(std::vector<std::string> &callLog) : calls{callLog}
            {
            }

            std::vector<std::string> &calls;
            bool started{true};
            bool prepared{true};
            bool shown{true};
            bool aborted{};
            std::optional<DropContext> context;

            [[nodiscard]] bool start() override
            {
                calls.emplace_back("thread");
                return started;
            }

            [[nodiscard]] bool prepare(std::span<const std::wstring>, Button, DropContext dropContext) override
            {
                calls.emplace_back("data");
                context = dropContext;
                return prepared;
            }

            [[nodiscard]] bool showAndArm() override
            {
                calls.emplace_back("show");
                return shown;
            }

            void abort() override
            {
                calls.emplace_back("abort");
                aborted = true;
            }

            [[nodiscard]] bool active() const override
            {
                return false;
            }

            void stop() override
            {
            }
        };

        tests::Panels readyPanels()
        {
            tests::Panels panels;
            panels.panels[0] = tests::visiblePanel({0, 0, 39, 24});
            panels.panels[0]->handle = 11;
            panels.panels[1] =
                PanelInfo{.visible = true, .plugin = true, .filePanel = true, .rect = {40, 0, 79, 24}, .handle = 22};
            panels.directories[0] = L"C:\\work";
            panels.directories[1] = L"D:\\target";
            panels.items[0] = {{.name = L"one.txt"}};
            return panels;
        }

        void arm(Session &session, Tool &tool)
        {
            REQUIRE(session.begin(tool, DragStart{Button::Left, {5, 5}}));
            REQUIRE(tool.context.has_value());
            session.prepare(*tool.context);
        }

    } // namespace

    TEST_SUITE("session")
    {
        TEST_CASE("begin drag keeps the 1.2.0 ordering")
        {
            auto panels = readyPanels();
            std::vector<std::string> calls;
            Screen screen{calls};
            Input input{calls};
            Tool tool{calls};
            tests::Host host;

            Session session{panels, host, screen, input};
            CHECK(session.begin(tool, DragStart{Button::Right, {5, 5}}));
            CHECK(calls == std::vector<std::string>{"thread", "button", "host", "geometry", "data", "button", "release",
                                                    "show"});
            REQUIRE(tool.context.has_value());
            CHECK(tool.context->press == Cell{5, 5});
            CHECK(tool.context->source == PanelSide::Active);
            CHECK(tool.context->panels[0]->rect == CellRect{0, 0, 39, 24});
            CHECK(tool.context->panels[1]->plugin);
            CHECK(tool.context->host->rect == PixelRect{0, 0, 100, 100});
            CHECK(tool.context->geometry == CellGeometry{{0, 0}, 1, 1});
            CHECK(tool.context->panelsWindow);
            CHECK(tool.context->sourcePaths == std::vector<std::wstring>{L"C:\\work\\one.txt"});
            CHECK(tool.context->destinationDirectory == L"D:\\target");
        }

        TEST_CASE("every gate fails without touching the button and prepared data is aborted")
        {
            SUBCASE("panels window")
            {
                auto panels = readyPanels();
                panels.panelsWindow = false;
                std::vector<std::string> calls;
                Screen screen{calls};
                Input input{calls};
                Tool tool{calls};
                tests::Host host;
                CHECK_FALSE(Session{panels, host, screen, input}.begin(tool, DragStart{Button::Left, {5, 5}}));
                CHECK(calls.empty());
            }

            SUBCASE("paths")
            {
                tests::Panels panels;
                std::vector<std::string> calls;
                Screen screen{calls};
                Input input{calls};
                Tool tool{calls};
                tests::Host host;
                CHECK_FALSE(Session{panels, host, screen, input}.begin(tool, DragStart{Button::Left, {5, 5}}));
                CHECK(calls.empty());
            }

            SUBCASE("thread")
            {
                auto panels = readyPanels();
                std::vector<std::string> calls;
                Screen screen{calls};
                Input input{calls};
                Tool tool{calls};
                tests::Host host;
                tool.started = false;
                CHECK_FALSE(Session{panels, host, screen, input}.begin(tool, DragStart{Button::Left, {5, 5}}));
                CHECK(calls == std::vector<std::string>{"thread"});
            }

            SUBCASE("data")
            {
                auto panels = readyPanels();
                std::vector<std::string> calls;
                Screen screen{calls};
                Input input{calls};
                Tool tool{calls};
                tests::Host host;
                tool.prepared = false;
                CHECK_FALSE(Session{panels, host, screen, input}.begin(tool, DragStart{Button::Left, {5, 5}}));
                CHECK(calls == std::vector<std::string>{"thread", "button", "host", "geometry", "data"});
            }

            SUBCASE("first button gate")
            {
                auto panels = readyPanels();
                std::vector<std::string> calls;
                Screen screen{calls};
                screen.buttons = {false};
                Input input{calls};
                Tool tool{calls};
                tests::Host host;
                CHECK_FALSE(Session{panels, host, screen, input}.begin(tool, DragStart{Button::Left, {5, 5}}));
                CHECK(calls == std::vector<std::string>{"thread", "button"});
            }

            SUBCASE("host")
            {
                auto panels = readyPanels();
                std::vector<std::string> calls;
                Screen screen{calls};
                screen.window = std::nullopt;
                Input input{calls};
                Tool tool{calls};
                tests::Host host;
                CHECK_FALSE(Session{panels, host, screen, input}.begin(tool, DragStart{Button::Left, {5, 5}}));
                CHECK(calls == std::vector<std::string>{"thread", "button", "host", "geometry", "data", "abort"});
            }

            SUBCASE("second button gate")
            {
                auto panels = readyPanels();
                std::vector<std::string> calls;
                Screen screen{calls};
                screen.buttons = {true, false};
                Input input{calls};
                Tool tool{calls};
                tests::Host host;
                CHECK_FALSE(Session{panels, host, screen, input}.begin(tool, DragStart{Button::Left, {5, 5}}));
                CHECK(calls ==
                      std::vector<std::string>{"thread", "button", "host", "geometry", "data", "button", "abort"});
            }
        }

        TEST_CASE("drop feedback queues a by-value request and Far synchro replays from fresh state")
        {
            auto panels = readyPanels();
            std::vector<std::string> calls;
            Screen screen{calls};
            Input input{calls};
            Tool tool{calls};
            tests::Host host;
            Session session{panels, host, screen, input};

            CHECK(session.effect({45, 5}, false) == Effect::None);
            CHECK(session.drop({45, 5}, false) == Effect::None);
            session.synchro();

            arm(session, tool);
            CHECK(session.effect({45, 5}, false) == Effect::Copy);
            CHECK(session.drop({45, 5}, true) == Effect::Move);
            CHECK(host.synchros == 1);
            CHECK(input.replays.empty());

            session.synchro();
            REQUIRE(input.replays.size() == 1);
            CHECK(screen.queriedPoint == Point{45, 5});
            CHECK(input.replays[0] ==
                  std::vector<MouseEvent>{MouseEvent{.at = {5, 5}, .left = true, .mods = {.shift = true}},
                                          MouseEvent{.at = {45, 5}, .mods = {.shift = true}}});
            CHECK(host.messages.empty());

            session.synchro();
            CHECK(input.replays.size() == 1);
            CHECK(session.drop({5, 5}, false) == Effect::None);
            CHECK(host.synchros == 1);
        }

        TEST_CASE("Far synchro cancels a queued drop when its source panel snapshot is stale")
        {
            auto panels = readyPanels();
            std::vector<std::string> calls;
            Screen screen{calls};
            Input input{calls};
            Tool tool{calls};
            tests::Host host;
            Session session{panels, host, screen, input};
            arm(session, tool);
            REQUIRE(session.drop({45, 5}, false) == Effect::Copy);

            SUBCASE("handle changed")
            {
                panels.panels[0]->handle = 99;
            }
            SUBCASE("rectangle changed")
            {
                panels.panels[0]->rect = {40, 0, 79, 24};
            }
            SUBCASE("source disappeared")
            {
                panels.panels[0] = std::nullopt;
            }
            SUBCASE("source is no longer a file panel")
            {
                panels.panels[0]->filePanel = false;
            }
            SUBCASE("source is hidden")
            {
                panels.panels[0]->visible = false;
            }
            SUBCASE("source directory changed")
            {
                panels.directories[0] = L"C:\\other";
            }
            SUBCASE("source selected names changed")
            {
                panels.items[0] = {{.name = L"two.txt"}};
            }
            SUBCASE("source current row changed")
            {
                panels.panels[0]->currentItem = 1;
            }
            SUBCASE("source top row changed")
            {
                panels.panels[0]->topItem = 1;
            }

            session.synchro();
            CHECK(input.replays.empty());
            REQUIRE(host.messages.size() == 1);
            REQUIRE(host.messages[0].size() == 1);
            CHECK(host.messages[0][0].find(L"changed") != std::wstring::npos);
        }

        TEST_CASE("Far synchro cancels when fresh destination geometry cannot accept the drop")
        {
            auto panels = readyPanels();
            std::vector<std::string> calls;
            Screen screen{calls};
            Input input{calls};
            Tool tool{calls};
            tests::Host host;
            Session session{panels, host, screen, input};
            arm(session, tool);
            REQUIRE(session.drop({45, 5}, false) == Effect::Copy);

            SUBCASE("destination moved")
            {
                panels.panels[1]->rect = {50, 0, 79, 24};
            }
            SUBCASE("destination changed type")
            {
                panels.panels[1]->filePanel = false;
            }
            SUBCASE("destination is hidden")
            {
                panels.panels[1]->visible = false;
            }
            SUBCASE("destination directory changed")
            {
                panels.directories[1] = L"E:\\other";
            }
            SUBCASE("destination current row changed")
            {
                panels.panels[1]->currentItem = 1;
            }
            SUBCASE("destination top row changed")
            {
                panels.panels[1]->topItem = 1;
            }
            SUBCASE("a panels window is no longer current")
            {
                panels.panelsWindow = false;
            }
            SUBCASE("host disappeared")
            {
                screen.pointWindow = std::nullopt;
            }
            SUBCASE("geometry disappeared")
            {
                screen.pointGeometry = std::unexpected(Error::Unavailable);
            }

            session.synchro();
            CHECK(input.replays.empty());
            CHECK(host.messages.size() == 1);
        }

        TEST_CASE("Far synchro requires the accepted drop cell under fresh point geometry")
        {
            auto panels = readyPanels();
            std::vector<std::string> calls;
            Screen screen{calls};
            Input input{calls};
            Tool tool{calls};
            tests::Host host;
            Session session{panels, host, screen, input};
            arm(session, tool);
            REQUIRE(session.drop({45, 5}, false) == Effect::Copy);
            screen.pointGeometry = CellGeometry{{1, 0}, 1, 1};

            session.synchro();
            CHECK(input.replays.empty());
            CHECK(host.messages.size() == 1);
        }

        TEST_CASE("Far synchro resolves the host from the recorded drop point")
        {
            auto panels = readyPanels();
            std::vector<std::string> calls;
            Screen screen{calls};
            Input input{calls};
            Tool tool{calls};
            tests::Host host;
            Session session{panels, host, screen, input};
            arm(session, tool);
            REQUIRE(session.drop({45, 5}, false) == Effect::Copy);
            screen.window = std::nullopt;

            session.synchro();
            CHECK(input.replays.size() == 1);
            CHECK(host.messages.empty());
        }

        TEST_CASE("Far synchro reports replay failures and releases a partial press")
        {
            auto panels = readyPanels();
            std::vector<std::string> calls;
            Screen screen{calls};
            Input input{calls};
            Tool tool{calls};
            tests::Host host;
            Session session{panels, host, screen, input};
            arm(session, tool);
            REQUIRE(session.drop({45, 5}, true) == Effect::Move);

            SUBCASE("nothing was written")
            {
                input.outcomes = {{0, 2, 0}};
                session.synchro();
                CHECK(input.replays.size() == 1);
            }
            SUBCASE("only the press was written")
            {
                input.outcomes = {{1, 2, 1}};
                session.synchro();
                REQUIRE(input.replays.size() == 2);
                CHECK(input.replays[1] == std::vector<MouseEvent>{MouseEvent{.at = {5, 5}, .mods = {.shift = true}}});
            }
            SUBCASE("the recovery release also failed")
            {
                input.outcomes = {{1, 2, 1}, {0, 1, 0}};
                session.synchro();
                CHECK(input.replays.size() == 2);
            }
            SUBCASE("the adapter reported a complete one-record request")
            {
                input.outcomes = {{1, 1, 1}};
                session.synchro();
                CHECK(input.replays.size() == 2);
            }

            REQUIRE(host.messages.size() == 1);
            REQUIRE(host.messages[0].size() == 1);
            CHECK(host.messages[0][0].find(L"input") != std::wstring::npos);
        }

        TEST_CASE("queued drop without a Far-thread source snapshot is rejected")
        {
            auto panels = readyPanels();
            std::vector<std::string> calls;
            Screen screen{calls};
            Input input{calls};
            tests::Host host;
            Session session{panels, host, screen, input};
            session.prepare(DropContext{.press = {5, 5},
                                        .source = PanelSide::Active,
                                        .panels = panels.panels,
                                        .host = HostWindow{1, {0, 0, 100, 100}, false},
                                        .geometry = CellGeometry{{0, 0}, 1, 1},
                                        .panelsWindow = true,
                                        .sourcePaths = {L"C:\\work\\one.txt"},
                                        .destinationDirectory = L"D:\\target"});
            REQUIRE(session.drop({45, 5}, false) == Effect::Copy);
            session.synchro();
            CHECK(input.replays.empty());
            CHECK(host.messages.size() == 1);
        }

        TEST_CASE("unavailable cell geometry does not prevent an external drag")
        {
            auto panels = readyPanels();
            std::vector<std::string> calls;
            Screen screen{calls};
            screen.geometry = std::unexpected(Error::Unavailable);
            Input input{calls};
            Tool tool{calls};
            tests::Host host;

            CHECK(Session{panels, host, screen, input}.begin(tool, DragStart{Button::Left, {5, 5}}));
            REQUIRE(tool.context.has_value());
            CHECK_FALSE(tool.context->geometry.has_value());
        }
    }

} // namespace burlak::core
