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
                return std::unexpected(Error::Unavailable);
            }
        };

        class Input final : public IInput
        {
          public:
            explicit Input(std::vector<std::string> &callLog) : calls{callLog}
            {
            }

            std::vector<std::string> &calls;

            void release(Button) override
            {
                calls.emplace_back("release");
            }

            void press(Button) override
            {
            }
            [[nodiscard]] ReplayOutcome replay(std::span<const MouseEvent>) override
            {
                return {true, 0, 0};
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

            [[nodiscard]] bool start() override
            {
                calls.emplace_back("thread");
                return started;
            }

            [[nodiscard]] bool prepare(std::span<const std::wstring>, Button) override
            {
                calls.emplace_back("data");
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
            panels.directories[0] = L"C:\\work";
            panels.items[0] = {{.name = L"one.txt"}};
            return panels;
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

            CHECK(Session{panels, screen, input, tool}.begin(Button::Right));
            CHECK(calls == std::vector<std::string>{"thread", "button", "data", "host", "button", "release", "show"});
        }

        TEST_CASE("every gate fails without touching the button and prepared data is aborted")
        {
            SUBCASE("paths")
            {
                tests::Panels panels;
                std::vector<std::string> calls;
                Screen screen{calls};
                Input input{calls};
                Tool tool{calls};
                CHECK_FALSE(Session{panels, screen, input, tool}.begin(Button::Left));
                CHECK(calls.empty());
            }

            SUBCASE("thread")
            {
                auto panels = readyPanels();
                std::vector<std::string> calls;
                Screen screen{calls};
                Input input{calls};
                Tool tool{calls};
                tool.started = false;
                CHECK_FALSE(Session{panels, screen, input, tool}.begin(Button::Left));
                CHECK(calls == std::vector<std::string>{"thread"});
            }

            SUBCASE("data")
            {
                auto panels = readyPanels();
                std::vector<std::string> calls;
                Screen screen{calls};
                Input input{calls};
                Tool tool{calls};
                tool.prepared = false;
                CHECK_FALSE(Session{panels, screen, input, tool}.begin(Button::Left));
                CHECK(calls == std::vector<std::string>{"thread", "button", "data"});
            }

            SUBCASE("first button gate")
            {
                auto panels = readyPanels();
                std::vector<std::string> calls;
                Screen screen{calls};
                screen.buttons = {false};
                Input input{calls};
                Tool tool{calls};
                CHECK_FALSE(Session{panels, screen, input, tool}.begin(Button::Left));
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
                CHECK_FALSE(Session{panels, screen, input, tool}.begin(Button::Left));
                CHECK(calls == std::vector<std::string>{"thread", "button", "data", "host", "abort"});
            }

            SUBCASE("second button gate")
            {
                auto panels = readyPanels();
                std::vector<std::string> calls;
                Screen screen{calls};
                screen.buttons = {true, false};
                Input input{calls};
                Tool tool{calls};
                CHECK_FALSE(Session{panels, screen, input, tool}.begin(Button::Left));
                CHECK(calls == std::vector<std::string>{"thread", "button", "data", "host", "button", "abort"});
            }
        }
    }

} // namespace burlak::core
