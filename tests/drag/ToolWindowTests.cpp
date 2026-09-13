#include "drag/ToolWindow.hpp"

#include "../Desktop.hpp"

#include <doctest/doctest.h>

#include <windows.h>

#include <functional>

namespace burlak::drag
{

    namespace
    {

        class Screen final : public core::IScreen
        {
          public:
            std::optional<core::HostWindow> host;
            bool down{true};

            [[nodiscard]] std::optional<core::Point> cursor() override
            {
                return {};
            }

            [[nodiscard]] bool buttonDown(core::Button) override
            {
                return down;
            }

            [[nodiscard]] std::optional<core::HostWindow> hostWindow() override
            {
                return host;
            }

            [[nodiscard]] std::expected<core::CellGeometry, core::Error> cellGeometry() override
            {
                return std::unexpected(core::Error::Unavailable);
            }
        };

        class Shell final : public core::IShell
        {
          public:
            class Data final : public DragData
            {
              public:
                [[nodiscard]] std::uintptr_t nativeHandle() const override
                {
                    return 1;
                }
            };

            bool prepares{true};
            int dragCalls{};
            std::function<void(core::NativeWindow)> duringDrag;

            [[nodiscard]] std::expected<std::unique_ptr<DragData>, core::Error> makeDataObject(
                std::span<const std::wstring>) override
            {
                if (!prepares) {
                    return std::unexpected(core::Error::NoSelection);
                }
                return std::make_unique<Data>();
            }

            [[nodiscard]] core::DragLoopOutcome runDrag(core::NativeWindow owner, DragData &, std::uintptr_t) override
            {
                ++dragCalls;
                if (duringDrag) {
                    duringDrag(owner);
                }
                return {};
            }

            [[nodiscard]] std::expected<void, core::Error> copy(std::span<const std::wstring>, std::wstring_view,
                                                                core::Effect) override
            {
                return {};
            }
        };

        class Input final : public core::IInput
        {
          public:
            std::vector<core::Button> presses;

            void release(core::Button) override
            {
            }
            void press(core::Button button) override
            {
                presses.push_back(button);
            }
            [[nodiscard]] core::ReplayOutcome replay(std::span<const core::MouseEvent>) override
            {
                return {true, 0, 0};
            }
        };

        class DropSession final : public core::IDropSession
        {
          public:
            std::optional<core::DropContext> context;

            void prepare(core::DropContext prepared) override
            {
                context = prepared;
            }
            [[nodiscard]] core::Effect effect(core::Point, bool) const override
            {
                return core::Effect::None;
            }
            [[nodiscard]] core::Effect drop(core::Point, bool) override
            {
                return core::Effect::None;
            }
        };

        core::DropContext dropContext()
        {
            return {.press = {5, 5},
                    .source = core::PanelSide::Active,
                    .panels = {},
                    .host = std::nullopt,
                    .geometry = std::nullopt,
                    .panelsWindow = false,
                    .sourcePaths = {},
                    .destinationDirectory = std::nullopt};
        }

        LRESULT CALLBACK hostProc(HWND window, UINT message, WPARAM word, LPARAM number)
        {
            return DefWindowProcW(window, message, word, number);
        }

        HWND hostWindow()
        {
            WNDCLASSW windowClass{};
            windowClass.lpfnWndProc = hostProc;
            windowClass.hInstance = GetModuleHandleW(nullptr);
            windowClass.lpszClassName = L"BurlakToolHostTest";
            static_cast<void>(RegisterClassW(&windowClass));
            return CreateWindowExW(0, windowClass.lpszClassName, L"", WS_POPUP | WS_VISIBLE, 100, 120, 320, 240,
                                   nullptr, nullptr, windowClass.hInstance, nullptr);
        }

        HANDLE WINAPI failCreateThread(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD)
        {
            return nullptr;
        }

        void WINAPI shortSleep(DWORD)
        {
            Sleep(1);
        }

        HWND WINAPI failCreateWindow(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID)
        {
            return nullptr;
        }

        BOOL WINAPI reportInvisible(HWND)
        {
            return FALSE;
        }

        struct HeadlessWindowState
        {
            bool visible{};
            int placements{};
        } headlessWindow;

        BOOL WINAPI headlessSetWindowPos(HWND, HWND, int, int, int, int, UINT flags)
        {
            ++headlessWindow.placements;
            if ((flags & SWP_SHOWWINDOW) != 0) {
                headlessWindow.visible = true;
            }
            return TRUE;
        }

        BOOL WINAPI headlessIsWindowVisible(HWND)
        {
            return headlessWindow.visible ? TRUE : FALSE;
        }

        BOOL WINAPI headlessShowWindow(HWND, int command)
        {
            if (command == SW_HIDE) {
                headlessWindow.visible = false;
            }
            return TRUE;
        }

        HWND WINAPI headlessSetCapture(HWND window)
        {
            return window;
        }

        BOOL WINAPI headlessReleaseCapture()
        {
            return TRUE;
        }

        UINT_PTR WINAPI headlessSetTimer(HWND, UINT_PTR event, UINT, TIMERPROC)
        {
            return event;
        }

        BOOL WINAPI headlessKillTimer(HWND, UINT_PTR)
        {
            return TRUE;
        }

        ToolWindowCalls headlessCalls()
        {
            auto calls = systemToolWindowCalls();
            calls.isWindowVisible = headlessIsWindowVisible;
            calls.setWindowPos = headlessSetWindowPos;
            calls.showWindow = headlessShowWindow;
            calls.setCapture = headlessSetCapture;
            calls.releaseCapture = headlessReleaseCapture;
            calls.setTimer = headlessSetTimer;
            calls.killTimer = headlessKillTimer;
            return calls;
        }

        void resetHeadlessWindow()
        {
            headlessWindow = {};
        }

    } // namespace

    TEST_SUITE("tool window")
    {
        TEST_CASE("thread, prepare, show, timer disarm, and stop use a headless window boundary")
        {
            resetHeadlessWindow();
            Screen screen;
            screen.host = core::HostWindow{1, {100, 120, 420, 360}, false};
            Input input;
            Shell shell;
            DropSession dropSession;
            const auto calls = headlessCalls();
            ToolWindow tool{screen, input, shell, dropSession, calls};

            CHECK(tool.start());
            CHECK(tool.start());
            REQUIRE(tool.nativeWindow() != 0);
            const std::vector<std::wstring> paths{L"C:\\one.txt"};
            CHECK(tool.prepare(paths, core::Button::Left, dropContext()));
            CHECK(tool.hasData());
            CHECK(dropSession.context->press == core::Cell{5, 5});
            CHECK(tool.showAndArm());
            CHECK(headlessWindow.visible);
            CHECK(headlessWindow.placements == 2);
            REQUIRE(input.presses.size() == 1);
            CHECK(input.presses[0] == core::Button::Left);

            SendMessageW(reinterpret_cast<HWND>(tool.nativeWindow()), WM_TIMER, armTimerId() + 1, 0);
            CHECK(headlessWindow.visible);
            SendMessageW(reinterpret_cast<HWND>(tool.nativeWindow()), WM_TIMER, armTimerId(), 0);
            CHECK_FALSE(headlessWindow.visible);
            CHECK_FALSE(tool.hasData());

            tool.stop();
            tool.stop();
            CHECK(tool.nativeWindow() == 0);
        }

        TEST_CASE("system placement can show the real tool window" *
                  doctest::skip(!burlak::tests::desktopAvailable(
                      "SKIP: tool-window integration requires a visible window station with cursor access\n")))
        {
            const HWND host = hostWindow();
            REQUIRE(host != nullptr);
            Screen screen;
            screen.host = core::HostWindow{reinterpret_cast<core::NativeWindow>(host), {100, 120, 420, 360}, false};
            Input input;
            Shell shell;
            DropSession dropSession;
            ToolWindow tool{screen, input, shell, dropSession};
            REQUIRE(tool.start());
            const std::vector<std::wstring> paths{L"C:\\one.txt"};
            REQUIRE(tool.prepare(paths, core::Button::Left, dropContext()));
            REQUIRE(tool.showAndArm());
            CHECK(IsWindowVisible(reinterpret_cast<HWND>(tool.nativeWindow())) != FALSE);
            tool.abort();
            tool.stop();
            DestroyWindow(host);
        }

        TEST_CASE("missing data or host aborts without injecting a press")
        {
            Screen screen;
            Input input;
            Shell shell;
            DropSession dropSession;
            ToolWindow tool{screen, input, shell, dropSession};
            REQUIRE(tool.start());
            CHECK_FALSE(tool.showAndArm());
            SendMessageW(reinterpret_cast<HWND>(tool.nativeWindow()), WM_LBUTTONDOWN, 0, 0);
            CHECK_FALSE(tool.active());

            shell.prepares = false;
            const std::vector<std::wstring> invalid{L"Z:\\definitely-missing\\file.txt"};
            CHECK_FALSE(tool.prepare(invalid, core::Button::Right, dropContext()));
            shell.prepares = true;
            const std::vector<std::wstring> valid{L"C:\\one.txt"};
            REQUIRE(tool.prepare(valid, core::Button::Right, dropContext()));
            CHECK_FALSE(tool.showAndArm());
            CHECK_FALSE(tool.hasData());
            CHECK(input.presses.empty());

            REQUIRE(tool.prepare(valid, core::Button::Right, dropContext()));
            tool.abort();
            CHECK_FALSE(tool.hasData());
            tool.stop();
        }

        TEST_CASE("placement decisions and both button messages run a headless shell loop")
        {
            resetHeadlessWindow();
            Screen screen;
            screen.host = core::HostWindow{1, {100, 120, 420, 360}, true};
            Input input;
            Shell shell;
            DropSession dropSession;
            const auto calls = headlessCalls();
            ToolWindow tool{screen, input, shell, dropSession, calls};
            REQUIRE(tool.start());
            const std::vector<std::wstring> paths{L"C:\\one.txt"};
            REQUIRE(tool.prepare(paths, core::Button::Right, dropContext()));
            REQUIRE(tool.showAndArm());
            CHECK(headlessWindow.placements == 1);
            const auto window = reinterpret_cast<HWND>(tool.nativeWindow());
            shell.duringDrag = [&tool](core::NativeWindow owner) {
                CHECK(tool.active());
                const auto window = reinterpret_cast<HWND>(owner);
                SendMessageW(window, WM_TIMER, armTimerId(), 0);
                SendMessageW(window, WM_RBUTTONDOWN, 0, 0);
            };
            SendMessageW(window, WM_RBUTTONDOWN, 0, 0);
            CHECK(shell.dragCalls == 1);
            CHECK_FALSE(tool.active());
            CHECK_FALSE(tool.hasData());
            CHECK_FALSE(headlessWindow.visible);

            screen.host->topmost = false;
            REQUIRE(tool.prepare(paths, core::Button::Left, dropContext()));
            REQUIRE(tool.showAndArm());
            CHECK(headlessWindow.placements == 3);
            shell.duringDrag = [&tool](core::NativeWindow owner) {
                CHECK(tool.active());
                SendMessageW(reinterpret_cast<HWND>(owner), WM_LBUTTONDOWN, 0, 0);
            };
            SendMessageW(window, WM_LBUTTONDOWN, 0, 0);
            CHECK(shell.dragCalls == 2);

            tool.stop();
        }

        TEST_CASE("thread and native-window startup failures remain expected")
        {
            Screen screen;
            Input input;
            Shell shell;
            DropSession dropSession;

            auto calls = systemToolWindowCalls();
            calls.createThread = failCreateThread;
            ToolWindow noThread{screen, input, shell, dropSession, calls};
            CHECK_FALSE(noThread.start());

            calls = systemToolWindowCalls();
            calls.createWindow = failCreateWindow;
            calls.sleep = shortSleep;
            ToolWindow noWindow{screen, input, shell, dropSession, calls};
            CHECK_FALSE(noWindow.start());
            noWindow.stop();
        }

        TEST_CASE("a headless window that cannot be shown drops prepared data")
        {
            resetHeadlessWindow();
            Screen screen;
            screen.host = core::HostWindow{1, {100, 120, 420, 360}, false};
            Input input;
            Shell shell;
            DropSession dropSession;
            auto calls = headlessCalls();
            calls.isWindowVisible = reportInvisible;
            ToolWindow tool{screen, input, shell, dropSession, calls};
            REQUIRE(tool.start());
            const std::vector<std::wstring> paths{L"C:\\one.txt"};
            REQUIRE(tool.prepare(paths, core::Button::Left, dropContext()));
            CHECK_FALSE(tool.showAndArm());
            CHECK_FALSE(tool.hasData());

            const HWND withoutState = CreateWindowExW(0, L"BurlakToolWindow", L"", WS_POPUP, 0, 0, 1, 1, nullptr,
                                                      nullptr, GetModuleHandleW(nullptr), nullptr);
            REQUIRE(withoutState != nullptr);
            DestroyWindow(withoutState);

            tool.stop();
        }
    }

} // namespace burlak::drag
