#include "drag/ToolWindow.hpp"

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

    } // namespace

    TEST_SUITE("tool window")
    {
        TEST_CASE("thread, prepare, show, timer disarm, and stop are real")
        {
            const HWND host = hostWindow();
            REQUIRE(host != nullptr);
            Screen screen;
            screen.host = core::HostWindow{reinterpret_cast<core::NativeWindow>(host), {100, 120, 420, 360}, false};
            Input input;
            Shell shell;
            ToolWindow tool{screen, input, shell};

            CHECK(tool.start());
            CHECK(tool.start());
            REQUIRE(tool.nativeWindow() != 0);
            const std::vector<std::wstring> paths{L"C:\\one.txt"};
            CHECK(tool.prepare(paths, core::Button::Left));
            CHECK(tool.hasData());
            CHECK(tool.showAndArm());
            CHECK(IsWindowVisible(reinterpret_cast<HWND>(tool.nativeWindow())) != FALSE);
            REQUIRE(input.presses.size() == 1);
            CHECK(input.presses[0] == core::Button::Left);

            SendMessageW(reinterpret_cast<HWND>(tool.nativeWindow()), WM_TIMER, armTimerId() + 1, 0);
            CHECK(IsWindowVisible(reinterpret_cast<HWND>(tool.nativeWindow())) != FALSE);
            SendMessageW(reinterpret_cast<HWND>(tool.nativeWindow()), WM_TIMER, armTimerId(), 0);
            CHECK(IsWindowVisible(reinterpret_cast<HWND>(tool.nativeWindow())) == FALSE);
            CHECK_FALSE(tool.hasData());

            tool.stop();
            tool.stop();
            CHECK(tool.nativeWindow() == 0);
            DestroyWindow(host);
        }

        TEST_CASE("missing data or host aborts without injecting a press")
        {
            Screen screen;
            Input input;
            Shell shell;
            ToolWindow tool{screen, input, shell};
            REQUIRE(tool.start());
            CHECK_FALSE(tool.showAndArm());
            SendMessageW(reinterpret_cast<HWND>(tool.nativeWindow()), WM_LBUTTONDOWN, 0, 0);
            CHECK_FALSE(tool.active());

            shell.prepares = false;
            const std::vector<std::wstring> invalid{L"Z:\\definitely-missing\\file.txt"};
            CHECK_FALSE(tool.prepare(invalid, core::Button::Right));
            shell.prepares = true;
            const std::vector<std::wstring> valid{L"C:\\one.txt"};
            REQUIRE(tool.prepare(valid, core::Button::Right));
            CHECK_FALSE(tool.showAndArm());
            CHECK_FALSE(tool.hasData());
            CHECK(input.presses.empty());

            REQUIRE(tool.prepare(valid, core::Button::Right));
            tool.abort();
            CHECK_FALSE(tool.hasData());
            tool.stop();
        }

        TEST_CASE("real placement calls and both button messages run a deterministic shell loop")
        {
            const HWND host = hostWindow();
            REQUIRE(host != nullptr);
            Screen screen;
            screen.host = core::HostWindow{reinterpret_cast<core::NativeWindow>(host), {100, 120, 420, 360}, true};
            Input input;
            Shell shell;
            ToolWindow tool{screen, input, shell};
            REQUIRE(tool.start());
            const std::vector<std::wstring> paths{L"C:\\one.txt"};
            REQUIRE(tool.prepare(paths, core::Button::Right));
            REQUIRE(tool.showAndArm());
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
            CHECK(IsWindowVisible(window) == FALSE);

            screen.host->topmost = false;
            REQUIRE(tool.prepare(paths, core::Button::Left));
            REQUIRE(tool.showAndArm());
            shell.duringDrag = [&tool](core::NativeWindow owner) {
                CHECK(tool.active());
                SendMessageW(reinterpret_cast<HWND>(owner), WM_LBUTTONDOWN, 0, 0);
            };
            SendMessageW(window, WM_LBUTTONDOWN, 0, 0);
            CHECK(shell.dragCalls == 2);

            tool.stop();
            DestroyWindow(host);
        }

        TEST_CASE("thread and native-window startup failures remain expected")
        {
            Screen screen;
            Input input;
            Shell shell;

            auto calls = systemToolWindowCalls();
            calls.createThread = failCreateThread;
            ToolWindow noThread{screen, input, shell, calls};
            CHECK_FALSE(noThread.start());

            calls = systemToolWindowCalls();
            calls.createWindow = failCreateWindow;
            calls.sleep = shortSleep;
            ToolWindow noWindow{screen, input, shell, calls};
            CHECK_FALSE(noWindow.start());
            noWindow.stop();
        }

        TEST_CASE("a window that cannot be shown drops prepared data")
        {
            const HWND host = hostWindow();
            REQUIRE(host != nullptr);
            Screen screen;
            screen.host = core::HostWindow{reinterpret_cast<core::NativeWindow>(host), {100, 120, 420, 360}, false};
            Input input;
            Shell shell;
            auto calls = systemToolWindowCalls();
            calls.isWindowVisible = reportInvisible;
            ToolWindow tool{screen, input, shell, calls};
            REQUIRE(tool.start());
            const std::vector<std::wstring> paths{L"C:\\one.txt"};
            REQUIRE(tool.prepare(paths, core::Button::Left));
            CHECK_FALSE(tool.showAndArm());
            CHECK_FALSE(tool.hasData());

            const HWND withoutState = CreateWindowExW(0, L"BurlakToolWindow", L"", WS_POPUP, 0, 0, 1, 1, nullptr,
                                                      nullptr, GetModuleHandleW(nullptr), nullptr);
            REQUIRE(withoutState != nullptr);
            DestroyWindow(withoutState);

            tool.stop();
            DestroyWindow(host);
        }
    }

} // namespace burlak::drag
