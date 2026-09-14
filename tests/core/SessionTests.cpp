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

            [[nodiscard]] NativeWindow windowAt(Point) override
            {
                return 0;
            }

            [[nodiscard]] NativeWindow consoleWindow() override
            {
                return 1;
            }

            [[nodiscard]] NativeWindow hostWindowHandle() override
            {
                return window.transform([](const HostWindow &host) { return host.handle; }).value_or(0);
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
            bool busy{};
            std::optional<DropContext> context;
            std::vector<std::wstring> paths;
            bool needsExtraction{};

            [[nodiscard]] bool start() override
            {
                calls.emplace_back("thread");
                return started;
            }

            [[nodiscard]] bool prepare(std::span<const std::wstring> preparedPaths, Button, bool extraction,
                                       DropContext dropContext) override
            {
                calls.emplace_back("data");
                paths.assign(preparedPaths.begin(), preparedPaths.end());
                needsExtraction = extraction;
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
                return busy;
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
            tests::Files files;
            tests::Shell shell;

            Session session{panels, host, screen, input, files, shell};
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
                tests::Files files;
                tests::Shell shell;
                CHECK_FALSE(
                    Session{panels, host, screen, input, files, shell}.begin(tool, DragStart{Button::Left, {5, 5}}));
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
                tests::Files files;
                tests::Shell shell;
                CHECK_FALSE(
                    Session{panels, host, screen, input, files, shell}.begin(tool, DragStart{Button::Left, {5, 5}}));
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
                tests::Files files;
                tests::Shell shell;
                tool.started = false;
                CHECK_FALSE(
                    Session{panels, host, screen, input, files, shell}.begin(tool, DragStart{Button::Left, {5, 5}}));
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
                tests::Files files;
                tests::Shell shell;
                tool.prepared = false;
                CHECK_FALSE(
                    Session{panels, host, screen, input, files, shell}.begin(tool, DragStart{Button::Left, {5, 5}}));
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
                tests::Files files;
                tests::Shell shell;
                CHECK_FALSE(
                    Session{panels, host, screen, input, files, shell}.begin(tool, DragStart{Button::Left, {5, 5}}));
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
                tests::Files files;
                tests::Shell shell;
                CHECK_FALSE(
                    Session{panels, host, screen, input, files, shell}.begin(tool, DragStart{Button::Left, {5, 5}}));
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
                tests::Files files;
                tests::Shell shell;
                CHECK_FALSE(
                    Session{panels, host, screen, input, files, shell}.begin(tool, DragStart{Button::Left, {5, 5}}));
                CHECK(calls ==
                      std::vector<std::string>{"thread", "button", "host", "geometry", "data", "button", "abort"});
            }

            SUBCASE("show")
            {
                auto panels = readyPanels();
                std::vector<std::string> calls;
                Screen screen{calls};
                Input input{calls};
                Tool tool{calls};
                tool.shown = false;
                tests::Host host;
                tests::Files files;
                tests::Shell shell;
                CHECK_FALSE(
                    Session{panels, host, screen, input, files, shell}.begin(tool, DragStart{Button::Left, {5, 5}}));
                CHECK(calls == std::vector<std::string>{"thread", "button", "host", "geometry", "data", "button",
                                                        "release", "show"});
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
            tests::Files files;
            tests::Shell shell;
            Session session{panels, host, screen, input, files, shell};

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

        TEST_CASE("ending a source drag clears the hover context so a later drop cannot replay a stale selection")
        {
            auto panels = readyPanels();
            std::vector<std::string> calls;
            Screen screen{calls};
            Input input{calls};
            Tool tool{calls};
            tests::Host host;
            tests::Files files;
            tests::Shell shell;
            Session session{panels, host, screen, input, files, shell};

            arm(session, tool);
            CHECK(session.effect({45, 5}, false) == Effect::Copy);

            session.endSource();
            CHECK(session.effect({45, 5}, false) == Effect::None);
            CHECK(session.drop({45, 5}, true) == Effect::None);
            CHECK(host.synchros == 0);
            session.synchro();
            CHECK(input.replays.empty());
        }

        TEST_CASE("a new gesture discards the replay queued by the previous drag")
        {
            auto panels = readyPanels();
            std::vector<std::string> calls;
            Screen screen{calls};
            Input input{calls};
            Tool tool{calls};
            tests::Host host;
            tests::Files files;
            tests::Shell shell;
            Session session{panels, host, screen, input, files, shell};

            arm(session, tool);
            CHECK(session.drop({45, 5}, false) == Effect::Copy);
            CHECK(host.synchros == 1);
            session.endSource();

            // Far consumed a new gesture past its threshold before servicing the queued replay synchro: the same
            // Far-thread call rebuilds the source context from the new press and must not then replay the old drop
            // against it.
            screen.buttons = {true, true};
            REQUIRE(session.begin(tool, DragStart{Button::Left, {6, 6}}));
            session.synchro();
            CHECK(input.replays.empty());
            CHECK(host.messages.empty());
        }

        TEST_CASE("Far synchro cancels a queued drop when its source panel snapshot is stale")
        {
            auto panels = readyPanels();
            std::vector<std::string> calls;
            Screen screen{calls};
            Input input{calls};
            Tool tool{calls};
            tests::Host host;
            tests::Files files;
            tests::Shell shell;
            Session session{panels, host, screen, input, files, shell};
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
            tests::Files files;
            tests::Shell shell;
            Session session{panels, host, screen, input, files, shell};
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
            tests::Files files;
            tests::Shell shell;
            Session session{panels, host, screen, input, files, shell};
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
            tests::Files files;
            tests::Shell shell;
            Session session{panels, host, screen, input, files, shell};
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
            tests::Files files;
            tests::Shell shell;
            Session session{panels, host, screen, input, files, shell};
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
            tests::Files files;
            tests::Shell shell;
            Session session{panels, host, screen, input, files, shell};
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
            tests::Files files;
            tests::Shell shell;

            CHECK(Session{panels, host, screen, input, files, shell}.begin(tool, DragStart{Button::Left, {5, 5}}));
            REQUIRE(tool.context.has_value());
            CHECK_FALSE(tool.context->geometry.has_value());
        }

        TEST_CASE("plugin-panel extraction is requested by value and completed on Far synchro")
        {
            auto panels = readyPanels();
            panels.panels[0]->realNames = false;
            panels.panels[0]->plugin = true;
            panels.panels[0]->owner[0] = std::byte{1};
            panels.items[0] = {
                {.name = L"one.txt", .size = 19, .attributes = 2, .selected = true, .userData = {.value = 23}},
                {.name = L""},
                {.name = L"."},
                {.name = L".."}};
            panels.panels[0]->selectedItems = panels.items[0].size();
            std::vector<std::string> calls;
            Screen screen{calls};
            Input input{calls};
            Tool tool{calls};
            tests::Host host;
            host.module = PluginModule{.path = L"Archive.dll", .instance = 42};
            tests::Files files;
            tests::Shell shell;
            Session session{panels, host, screen, input, files, shell};

            REQUIRE(session.begin(tool, DragStart{Button::Left, {5, 5}}));
            CHECK(tool.needsExtraction);
            CHECK(tool.paths == std::vector<std::wstring>{L"C:\\Temp\\Burlak\\7-1\\one.txt"});
            REQUIRE(session.requestExtraction());
            CHECK_FALSE(session.requestExtraction());
            CHECK(host.synchros == 1);
            CHECK(host.extractedPanels.empty());

            const auto result = session.synchro();
            REQUIRE(result.has_value());
            CHECK(*result);
            CHECK(host.extractedPanels == std::vector<PanelHandle>{11});
            CHECK(
                host.extractedItems ==
                std::vector<std::vector<Item>>{
                    {{.name = L"one.txt", .size = 19, .attributes = 2, .selected = true, .userData = {.value = 23}}}});
            CHECK(host.extractedModules[0].path == L"Archive.dll");
            CHECK(host.extractionDirectories == std::vector<std::wstring>{L"C:\\Temp\\Burlak\\7-1"});
            CHECK(session.synchro() == std::nullopt);

            session.cleanup();
            CHECK(files.removed == std::vector<std::wstring>{L"C:\\Temp\\Burlak\\7-1"});
            CHECK(files.touched.empty());
            session.cleanup();
            CHECK(files.removed.size() == 1);
        }

        TEST_CASE("a plugin placeholder run is discarded when the drag thread cannot start")
        {
            auto panels = readyPanels();
            panels.panels[0]->realNames = false;
            panels.panels[0]->plugin = true;
            panels.panels[0]->owner[0] = std::byte{1};
            panels.panels[0]->selectedItems = panels.items[0].size();
            std::vector<std::string> calls;
            Screen screen{calls};
            Input input{calls};
            Tool tool{calls};
            tool.started = false;
            tests::Host host;
            host.module = PluginModule{.path = L"Archive.dll", .instance = 42};
            tests::Files files;
            tests::Shell shell;

            CHECK_FALSE(
                Session{panels, host, screen, input, files, shell}.begin(tool, DragStart{Button::Left, {5, 5}}));
            CHECK(files.removed == std::vector<std::wstring>{L"C:\\Temp\\Burlak\\7-1"});
        }

        TEST_CASE("retaining an extraction restarts its grace clock and detaches cleanup")
        {
            auto panels = readyPanels();
            panels.panels[0]->realNames = false;
            panels.panels[0]->plugin = true;
            panels.panels[0]->owner[0] = std::byte{1};
            panels.panels[0]->selectedItems = panels.items[0].size();
            std::vector<std::string> calls;
            Screen screen{calls};
            Input input{calls};
            Tool tool{calls};
            tests::Host host;
            host.module = PluginModule{.path = L"Archive.dll", .instance = 42};
            tests::Files files;
            files.touchResult = std::unexpected(Error::Unavailable);
            tests::Shell shell;
            Session session{panels, host, screen, input, files, shell};

            REQUIRE(session.begin(tool, DragStart{Button::Left, {5, 5}}));
            session.retain();
            session.cleanup();
            CHECK(files.touched == std::vector<std::wstring>{L"C:\\Temp\\Burlak\\7-1"});
            CHECK(files.removed.empty());
            session.retain();
            CHECK(files.touched.size() == 1);
        }

        TEST_CASE("a same-Far plugin-panel drop replays Far's copy without extracting placeholders")
        {
            auto panels = readyPanels();
            panels.panels[0]->realNames = false;
            panels.panels[0]->plugin = true;
            panels.panels[0]->owner[0] = std::byte{1};
            panels.items[0] = {{.name = L"one.txt", .size = 19, .attributes = 2, .selected = true},
                               {.name = L""},
                               {.name = L"."},
                               {.name = L".."}};
            panels.panels[0]->selectedItems = panels.items[0].size();
            std::vector<std::string> calls;
            Screen screen{calls};
            Input input{calls};
            Tool tool{calls};
            tests::Host host;
            host.module = PluginModule{.path = L"Archive.dll", .instance = 42};
            tests::Files files;
            tests::Shell shell;
            Session session{panels, host, screen, input, files, shell};
            arm(session, tool);
            REQUIRE(session.drop({45, 5}, false) == Effect::Copy);

            SUBCASE("the selection is unchanged")
            {
                session.synchro();
                CHECK(input.replays.size() == 1);
                CHECK(host.extractedPanels.empty());
            }
            SUBCASE("the selection changed")
            {
                panels.items[0][0].size = 20;
                session.synchro();
                CHECK(input.replays.empty());
                CHECK(host.extractedPanels.empty());
                CHECK(host.messages.size() == 1);
            }
            SUBCASE("the selected-item snapshot is incomplete")
            {
                panels.panels[0]->selectedItems = panels.items[0].size() + 1;
                session.synchro();
                CHECK(input.replays.empty());
                CHECK(host.extractedPanels.empty());
                CHECK(host.messages.size() == 1);
            }
            SUBCASE("the source panel disappeared")
            {
                panels.panels[0].reset();
                session.synchro();
                CHECK(input.replays.empty());
                CHECK(host.extractedPanels.empty());
                CHECK(host.messages.size() == 1);
            }
        }

        TEST_CASE("plugin refusal cancels extraction with a one-line message naming its module")
        {
            auto panels = readyPanels();
            panels.panels[0]->realNames = false;
            panels.panels[0]->plugin = true;
            panels.panels[0]->owner[0] = std::byte{1};
            panels.panels[0]->selectedItems = panels.items[0].size();
            std::vector<std::string> calls;
            Screen screen{calls};
            Input input{calls};
            Tool tool{calls};
            tests::Host host;
            std::wstring modulePath;
            SUBCASE("qualified module path")
            {
                modulePath = L"C:\\Plugins\\NetBox.dll";
            }
            SUBCASE("bare module name")
            {
                modulePath = L"NetBox.dll";
            }
            host.module = PluginModule{.path = modulePath, .instance = 42};
            host.extractionResult = std::unexpected(Error::ForeignCallFailed);
            tests::Files files;
            tests::Shell shell;
            Session session{panels, host, screen, input, files, shell};

            CHECK_FALSE(session.requestExtraction());
            REQUIRE(session.begin(tool, DragStart{Button::Left, {5, 5}}));
            REQUIRE(session.requestExtraction());
            CHECK(session.synchro() == std::optional{false});
            REQUIRE(host.messages.size() == 1);
            REQUIRE(host.messages[0].size() == 1);
            CHECK(host.messages[0][0].find(L"NetBox.dll") != std::wstring::npos);
        }

        TEST_CASE("a plugin destination rewrite cancels the drop because its advertised paths are stale")
        {
            auto panels = readyPanels();
            panels.panels[0]->realNames = false;
            panels.panels[0]->plugin = true;
            panels.panels[0]->owner[0] = std::byte{1};
            panels.panels[0]->selectedItems = panels.items[0].size();
            std::vector<std::string> calls;
            Screen screen{calls};
            Input input{calls};
            Tool tool{calls};
            tests::Host host;
            host.module = PluginModule{.path = L"C:\\Plugins\\NetBox.dll", .instance = 42};
            host.extractionResult = std::wstring{L"C:\\somewhere-else"};
            tests::Files files;
            tests::Shell shell;
            Session session{panels, host, screen, input, files, shell};

            REQUIRE(session.begin(tool, DragStart{Button::Left, {5, 5}}));
            REQUIRE(session.requestExtraction());
            CHECK(session.synchro() == std::optional{false});
            REQUIRE(host.messages.size() == 1);
            REQUIRE(host.messages[0].size() == 1);
            CHECK(host.messages[0][0].find(L"NetBox.dll") != std::wstring::npos);
        }

        TEST_CASE("plugin extraction revalidates the panel selection owner and module on Far's thread")
        {
            auto panels = readyPanels();
            panels.panels[0]->realNames = false;
            panels.panels[0]->plugin = true;
            panels.panels[0]->owner[0] = std::byte{1};
            panels.items[0] = {{.name = L"one.txt", .size = 19, .selected = true}};
            panels.panels[0]->selectedItems = panels.items[0].size();
            std::vector<std::string> calls;
            Screen screen{calls};
            Input input{calls};
            Tool tool{calls};
            tests::Host host;
            host.module = PluginModule{.path = L"Archive.dll", .instance = 42};
            tests::Files files;
            tests::Shell shell;
            Session session{panels, host, screen, input, files, shell};
            REQUIRE(session.begin(tool, DragStart{Button::Left, {5, 5}}));
            REQUIRE(session.requestExtraction());

            SUBCASE("panel was closed")
            {
                panels.panels[0].reset();
            }
            SUBCASE("panel handle changed")
            {
                panels.panels[0]->handle = 99;
            }
            SUBCASE("panel is hidden")
            {
                panels.panels[0]->visible = false;
            }
            SUBCASE("panel is no longer a plugin")
            {
                panels.panels[0]->plugin = false;
            }
            SUBCASE("panel is no longer a file panel")
            {
                panels.panels[0]->filePanel = false;
            }
            SUBCASE("panel owner changed")
            {
                panels.panels[0]->owner[1] = std::byte{2};
            }
            SUBCASE("panel stopped being a virtual plugin panel")
            {
                panels.panels[0]->realNames = true;
            }
            SUBCASE("a non-panel window became current")
            {
                panels.panelsWindow = false;
            }
            SUBCASE("selection changed")
            {
                panels.items[0][0].size = 20;
            }
            SUBCASE("a pointer-backed selection field changed")
            {
                panels.items[0][0].identity = {std::byte{1}};
            }
            SUBCASE("the selected-item snapshot became incomplete")
            {
                panels.panels[0]->selectedItems = 2;
            }
            SUBCASE("owner module disappeared")
            {
                host.module.reset();
            }
            SUBCASE("owner module instance changed")
            {
                host.module->instance = 43;
            }

            CHECK(session.synchro() == std::optional{false});
            CHECK(host.extractedPanels.empty());
            CHECK(host.messages.size() == 1);
        }

        TEST_CASE("real-path plans neither request extraction nor ask the tool to clean temporary files")
        {
            auto panels = readyPanels();
            std::vector<std::string> calls;
            Screen screen{calls};
            Input input{calls};
            Tool tool{calls};
            tests::Host host;
            tests::Files files;
            tests::Shell shell;
            Session session{panels, host, screen, input, files, shell};

            REQUIRE(session.begin(tool, DragStart{Button::Left, {5, 5}}));
            CHECK(files.sweeps == 0);
            CHECK_FALSE(tool.needsExtraction);
            CHECK_FALSE(session.requestExtraction());
            session.retain();
            CHECK(files.touched.empty());
            session.cleanup();
            CHECK(files.removed.empty());
        }

        TEST_CASE("a busy tool window refuses a source session before cleanup")
        {
            auto panels = readyPanels();
            std::vector<std::string> calls;
            Screen screen{calls};
            Input input{calls};
            Tool tool{calls};
            tool.busy = true;
            tests::Host host;
            tests::Files files;
            tests::Shell shell;
            Session session{panels, host, screen, input, files, shell};

            CHECK_FALSE(session.begin(tool, DragStart{Button::Left, {5, 5}}));
            CHECK(calls.empty());
            CHECK(files.sweeps == 0);
        }

        TEST_CASE("external receive snapshot, shell copy, and redraw cross the synchro boundary by value")
        {
            auto panels = readyPanels();
            panels.panels[1]->realNames = true;
            std::vector<std::string> calls;
            Screen screen{calls};
            Input input{calls};
            tests::Host host;
            tests::Files files;
            tests::Shell shell;
            Session session{panels, host, screen, input, files, shell};

            REQUIRE(session.requestReceiveSnapshot({45, 5}));
            CHECK_FALSE(session.requestReceiveSnapshot({46, 6}));
            CHECK(host.synchros == 1);
            CHECK_FALSE(session.takeReceiveSnapshot().has_value());
            session.synchro();
            auto snapshot = session.takeReceiveSnapshot();
            REQUIRE(snapshot.has_value());
            CHECK(snapshot->panels == panels.panels);
            CHECK(snapshot->directories == panels.directories);
            CHECK(snapshot->host == screen.pointWindow);
            CHECK(snapshot->geometry == std::optional{*screen.pointGeometry});
            CHECK_FALSE(session.takeReceiveSnapshot().has_value());

            screen.pointGeometry = std::unexpected(Error::Unavailable);
            REQUIRE(session.requestReceiveSnapshot({47, 7}));
            session.synchro();
            const auto withoutGeometry = session.takeReceiveSnapshot();
            REQUIRE(withoutGeometry.has_value());
            CHECK_FALSE(withoutGeometry->geometry.has_value());

            session.prepareReceive(*snapshot);
            CHECK(session.receiveOwner() == 1);
            CHECK(session.receiveEffect({45, 5}, false) == Effect::Copy);
            CHECK(session.receiveEffect({45, 5}, true) == Effect::Move);
            const std::vector<std::wstring> paths{L"C:\\source\\one.txt"};
            CHECK(session.receiveDrop(paths, {45, 5}, Effect::Copy) == ReceiveDropOutcome{Effect::Copy, false});
            REQUIRE(shell.copies.size() == 1);
            CHECK(shell.destinations == std::vector<std::wstring>{L"D:\\target"});
            CHECK(shell.owners == std::vector<NativeWindow>{1});
            CHECK(host.synchros == 3);
            CHECK(panels.updates.empty());
            session.synchro();
            CHECK(panels.updates == std::vector<PanelSide>{PanelSide::Passive});

            session.cancelReceive();
            CHECK(session.receiveOwner() == 0);
            CHECK(session.receiveEffect({45, 5}, false) == Effect::None);
            CHECK(session.receiveDrop(paths, {45, 5}, Effect::Copy) == ReceiveDropOutcome{});
        }

        TEST_CASE("failed, empty, and invalid-effect receives do not redraw")
        {
            auto panels = readyPanels();
            panels.panels[1]->realNames = true;
            std::vector<std::string> calls;
            Screen screen{calls};
            Input input{calls};
            tests::Host host;
            tests::Files files;
            tests::Shell shell;
            shell.copyResult = std::unexpected(Error::ForeignCallFailed);
            Session session{panels, host, screen, input, files, shell};
            session.prepareReceive(ReceiveSnapshot{.panelsWindow = true,
                                                   .panels = panels.panels,
                                                   .directories = panels.directories,
                                                   .host = screen.pointWindow,
                                                   .geometry = *screen.pointGeometry});
            const std::vector<std::wstring> paths{L"C:\\source\\one.txt"};

            CHECK(session.receiveDrop(paths, {45, 5}, Effect::Move) == ReceiveDropOutcome{});
            CHECK(session.receiveDrop({}, {45, 5}, Effect::Copy) == ReceiveDropOutcome{});
            CHECK(session.receiveDrop(paths, {45, 5}, Effect::None) == ReceiveDropOutcome{});
            CHECK(session.receiveDrop(paths, {5, 0}, Effect::Copy) == ReceiveDropOutcome{});
            CHECK(host.synchros == 0);
            CHECK(panels.updates.empty());
        }

        TEST_CASE("a receive refresh accepts the same panel identity and rejects a changed destination")
        {
            auto panels = readyPanels();
            panels.panels[1]->realNames = true;
            std::vector<std::string> calls;
            Screen screen{calls};
            Input input{calls};
            tests::Host host;
            tests::Files files;
            tests::Shell shell;
            Session session{panels, host, screen, input, files, shell};
            const ReceiveSnapshot initial{.panelsWindow = true,
                                          .panels = panels.panels,
                                          .directories = panels.directories,
                                          .host = screen.pointWindow,
                                          .geometry = *screen.pointGeometry};
            CHECK_FALSE(session.requestReceiveRefresh({45, 5}));
            session.prepareReceive(initial);

            REQUIRE(session.requestReceiveRefresh({45, 5}));
            CHECK_FALSE(session.requestReceiveRefresh({46, 6}));
            session.synchro();
            CHECK(session.takeReceiveRefresh() == std::optional{true});
            CHECK_FALSE(session.takeReceiveRefresh().has_value());
            CHECK(host.messages.empty());

            panels.directories[1] = L"D:\\changed";
            REQUIRE(session.requestReceiveRefresh({45, 5}));
            session.synchro();
            CHECK(session.takeReceiveRefresh() == std::optional{false});
            REQUIRE(host.messages.size() == 1);
            CHECK(host.messages[0] ==
                  std::vector<std::wstring>{L"Panel changed during the drop; the drop was cancelled."});
            panels.directories[1] = initial.directories[1];

            SUBCASE("a background refresh of the panel not receiving the drop is accepted")
            {
                panels.panels[0]->currentItem = 7;
                panels.panels[0]->topItem = 3;
                panels.panels[0]->selectedItems = 9;
                panels.directories[0] = L"E:\\elsewhere";
                panels.panels[1]->currentItem = 2;
                REQUIRE(session.requestReceiveRefresh({45, 5}));
                session.synchro();
                CHECK(session.takeReceiveRefresh() == std::optional{true});
                CHECK(host.messages.size() == 1);
            }
            SUBCASE("the destination panel hidden during the drop cancels it")
            {
                panels.panels[1]->visible = false;
                REQUIRE(session.requestReceiveRefresh({45, 5}));
                session.synchro();
                CHECK(session.takeReceiveRefresh() == std::optional{false});
                CHECK(host.messages.size() == 2);
            }
            SUBCASE("cell geometry that disappears during the drop cancels it")
            {
                screen.pointGeometry = std::unexpected(Error::Unavailable);
                REQUIRE(session.requestReceiveRefresh({45, 5}));
                session.synchro();
                CHECK(session.takeReceiveRefresh() == std::optional{false});
                CHECK(host.messages.size() == 2);
            }
            SUBCASE("the refresh answers for the snapshot that was current when Drop asked")
            {
                panels.directories[1] = L"D:\\changed";
                REQUIRE(session.requestReceiveRefresh({45, 5}));
                auto replaced = initial;
                replaced.directories[1] = L"D:\\changed";
                session.prepareReceive(replaced);
                session.synchro();
                CHECK(session.takeReceiveRefresh() == std::optional{false});
                CHECK(host.messages.size() == 2);
            }
            SUBCASE("a cancelled receive drops its pending refresh")
            {
                REQUIRE(session.requestReceiveRefresh({45, 5}));
                session.cancelReceive();
                session.synchro();
                CHECK_FALSE(session.takeReceiveRefresh().has_value());
                CHECK(host.messages.size() == 1);
            }
        }
    }

} // namespace burlak::core
