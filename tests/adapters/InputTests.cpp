#include "adapters/win/Input.hpp"

#include "../Desktop.hpp"
#include "core/Policies.hpp"

#include <doctest/doctest.h>

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <vector>

namespace burlak::adapters::win
{

    namespace
    {

        struct Clicks
        {
            int leftDown{};
            int leftUp{};
            int rightDown{};
            int rightUp{};
        } clicks;

        std::vector<DWORD> injectedFlags;
        std::vector<INPUT_RECORD> replayedRecords;
        int replayWrites{};

        void WINAPI recordMouseEvent(DWORD flags, DWORD, DWORD, DWORD, ULONG_PTR)
        {
            injectedFlags.push_back(flags);
        }

        BOOL WINAPI partialWrite(HANDLE, const INPUT_RECORD *, DWORD requested, LPDWORD written)
        {
            *written = requested - 1;
            return TRUE;
        }

        BOOL WINAPI tallConsoleInfo(HANDLE, PCONSOLE_SCREEN_BUFFER_INFO info)
        {
            *info = {};
            info->dwSize.Y = 9001;
            info->srWindow.Top = 20;
            info->srWindow.Bottom = 69;
            return TRUE;
        }

        BOOL WINAPI unavailableConsoleInfo(HANDLE, PCONSOLE_SCREEN_BUFFER_INFO)
        {
            return FALSE;
        }

        BOOL WINAPI degenerateConsoleInfo(HANDLE, PCONSOLE_SCREEN_BUFFER_INFO info)
        {
            *info = {};
            return TRUE;
        }

        BOOL WINAPI captureReplay(HANDLE, const INPUT_RECORD *records, DWORD requested, LPDWORD written)
        {
            ++replayWrites;
            replayedRecords.assign(records, records + requested);
            *written = requested;
            return TRUE;
        }

        bool visibleWindowStation()
        {
            return burlak::tests::desktopAvailable(
                "SKIP: injected-click integration requires a visible window station with cursor access\n");
        }

        class CursorGuard
        {
          public:
            CursorGuard() : captured_{GetCursorPos(&position_) != FALSE}
            {
            }
            ~CursorGuard()
            {
                if (captured_) {
                    static_cast<void>(SetCursorPos(position_.x, position_.y));
                }
            }
            CursorGuard(const CursorGuard &) = delete;
            CursorGuard &operator=(const CursorGuard &) = delete;
            [[nodiscard]] bool captured() const
            {
                return captured_;
            }
            [[nodiscard]] POINT position() const
            {
                return position_;
            }

          private:
            POINT position_{};
            bool captured_{};
        };

        LRESULT CALLBACK clickWindowProcedure(HWND window, UINT message, WPARAM word, LPARAM number)
        {
            if (message == WM_LBUTTONDOWN) {
                ++clicks.leftDown;
            } else if (message == WM_LBUTTONUP) {
                ++clicks.leftUp;
            } else if (message == WM_RBUTTONDOWN) {
                ++clicks.rightDown;
            } else if (message == WM_RBUTTONUP) {
                ++clicks.rightUp;
            }
            return DefWindowProcW(window, message, word, number);
        }

        HWND clickWindow(POINT position)
        {
            WNDCLASSW windowClass{};
            windowClass.lpfnWndProc = clickWindowProcedure;
            windowClass.hInstance = GetModuleHandleW(nullptr);
            windowClass.lpszClassName = L"BurlakInjectedClickTest";
            static_cast<void>(RegisterClassW(&windowClass));
            return CreateWindowExW(WS_EX_TOPMOST, windowClass.lpszClassName, L"", WS_POPUP | WS_VISIBLE,
                                   position.x - 20, position.y - 20, 40, 40, nullptr, nullptr, windowClass.hInstance,
                                   nullptr);
        }

        class WindowGuard
        {
          public:
            explicit WindowGuard(POINT position) : value_{clickWindow(position)}
            {
            }
            ~WindowGuard()
            {
                if (value_ != nullptr) {
                    DestroyWindow(value_);
                }
            }
            WindowGuard(const WindowGuard &) = delete;
            WindowGuard &operator=(const WindowGuard &) = delete;
            [[nodiscard]] HWND get() const
            {
                return value_;
            }

          private:
            HWND value_{};
        };

        [[nodiscard]] bool clicksComplete()
        {
            return clicks.leftDown > 0 && clicks.leftUp > 0 && clicks.rightDown > 0 && clicks.rightUp > 0;
        }

        bool establishClickPoint(HWND window, POINT requested)
        {
            if (SetCursorPos(requested.x, requested.y) == FALSE) {
                return false;
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
            do {
                POINT positioned{};
                if (GetCursorPos(&positioned) != FALSE && positioned.x == requested.x && positioned.y == requested.y &&
                    WindowFromPoint(positioned) == window && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0 &&
                    (GetAsyncKeyState(VK_RBUTTON) & 0x8000) == 0) {
                    return true;
                }
                static_cast<void>(MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT));
            } while (std::chrono::steady_clock::now() < deadline);
            return false;
        }

        void pumpUntilClicksComplete()
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
            while (!clicksComplete() && std::chrono::steady_clock::now() < deadline) {
                static_cast<void>(MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT));
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    static_cast<void>(TranslateMessage(&message));
                    static_cast<void>(DispatchMessageW(&message));
                }
            }
        }

    } // namespace

    TEST_SUITE("input adapter")
    {
        TEST_CASE("BURLAK_NO_DESKTOP disables desktop integration through the shared predicate")
        {
            wchar_t previous[16]{};
            const DWORD previousLength = GetEnvironmentVariableW(L"BURLAK_NO_DESKTOP", previous, 16);
            REQUIRE(SetEnvironmentVariableW(L"BURLAK_NO_DESKTOP", L"1") != FALSE);
            CHECK_FALSE(burlak::tests::desktopAvailable("unused\n"));
            REQUIRE(SetEnvironmentVariableW(L"BURLAK_NO_DESKTOP",
                                            previousLength > 0 && previousLength < 16 ? previous : nullptr) != FALSE);
        }

        TEST_CASE("console replay writes mapped mouse records to the process console")
        {
            const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
            DWORD mode{};
            if (input == nullptr || input == INVALID_HANDLE_VALUE || !GetConsoleMode(input, &mode)) {
                static_cast<void>(FreeConsole());
                REQUIRE(AllocConsole());
            }
            const HANDLE consoleInput =
                CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, 0, nullptr);
            REQUIRE(consoleInput != INVALID_HANDLE_VALUE);
            const HANDLE consoleOutput =
                CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, 0, nullptr);
            REQUIRE(consoleOutput != INVALID_HANDLE_VALUE);
            FlushConsoleInputBuffer(consoleInput);

            Input adapter{reinterpret_cast<core::NativeWindow>(consoleInput),
                          reinterpret_cast<core::NativeWindow>(consoleOutput), systemInputCalls()};
            const std::vector<core::MouseEvent> events{
                {.at = {7, 8}, .left = true, .moved = true, .mods = {.shift = true}},
                {.at = {9, 10}, .right = true, .wheel = true, .mods = {.control = true, .alt = true}}};
            const auto replay = adapter.replay(events);
            REQUIRE(replay == core::ReplayOutcome{true, 2, 2});
            REQUIRE(core::replayOutcome(replay).has_value());

            INPUT_RECORD records[2]{};
            DWORD read{};
            REQUIRE(ReadConsoleInputW(consoleInput, records, 2, &read));
            REQUIRE(read == 2);
            CHECK(records[0].Event.MouseEvent.dwMousePosition.X == 7);
            CHECK(records[0].Event.MouseEvent.dwButtonState == FROM_LEFT_1ST_BUTTON_PRESSED);
            CHECK(records[0].Event.MouseEvent.dwEventFlags == MOUSE_MOVED);
            CHECK((records[0].Event.MouseEvent.dwControlKeyState & SHIFT_PRESSED) != 0);
            CHECK(records[1].Event.MouseEvent.dwButtonState == RIGHTMOST_BUTTON_PRESSED);
            CHECK(records[1].Event.MouseEvent.dwEventFlags == MOUSE_WHEELED);
            CHECK((records[1].Event.MouseEvent.dwControlKeyState & LEFT_CTRL_PRESSED) != 0);
            CHECK((records[1].Event.MouseEvent.dwControlKeyState & LEFT_ALT_PRESSED) != 0);
            CloseHandle(consoleOutput);
            CloseHandle(consoleInput);
        }

        TEST_CASE("an unavailable console is an expected replay failure")
        {
            const std::vector<core::MouseEvent> event{{}};
            Input nullInput{0};
            CHECK(core::replayOutcome(nullInput.replay(event)) == std::unexpected(core::Error::Unavailable));
            Input invalidInput{reinterpret_cast<core::NativeWindow>(INVALID_HANDLE_VALUE)};
            CHECK(core::replayOutcome(invalidInput.replay(event)) == std::unexpected(core::Error::Unavailable));

            const HANDLE file =
                CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            REQUIRE(file != INVALID_HANDLE_VALUE);
            Input notConsole{reinterpret_cast<core::NativeWindow>(file)};
            CHECK(core::replayOutcome(notConsole.replay(event)) == std::unexpected(core::Error::Unavailable));
            CloseHandle(file);
        }

        TEST_CASE("console replay adds Far's window-mode row offset and preserves every other field")
        {
            auto calls = systemInputCalls();
            calls.getConsoleScreenBufferInfo = tallConsoleInfo;
            calls.writeConsoleInput = captureReplay;
            Input input{1, 2, calls};
            const std::vector<core::MouseEvent> events{{.at = {5, 5}, .left = true, .mods = {.shift = true}},
                                                       {.at = {45, 6}, .mods = {.shift = true}}};
            replayedRecords.clear();
            replayWrites = 0;

            CHECK(input.replay(events) == core::ReplayOutcome{true, 2, 2});
            REQUIRE(replayedRecords.size() == 2);
            CHECK(replayedRecords[0].Event.MouseEvent.dwMousePosition.X == 5);
            CHECK(replayedRecords[0].Event.MouseEvent.dwMousePosition.Y == 8956);
            CHECK(replayedRecords[0].Event.MouseEvent.dwButtonState == FROM_LEFT_1ST_BUTTON_PRESSED);
            CHECK(replayedRecords[0].Event.MouseEvent.dwControlKeyState == SHIFT_PRESSED);
            CHECK(replayedRecords[0].Event.MouseEvent.dwEventFlags == 0);
            CHECK(replayedRecords[1].Event.MouseEvent.dwMousePosition.X == 45);
            CHECK(replayedRecords[1].Event.MouseEvent.dwMousePosition.Y == 8957);
            CHECK(replayedRecords[1].Event.MouseEvent.dwButtonState == 0);
            CHECK(replayedRecords[1].Event.MouseEvent.dwControlKeyState == SHIFT_PRESSED);
            CHECK(replayedRecords[1].Event.MouseEvent.dwEventFlags == 0);
        }

        TEST_CASE("console replay writes nothing when buffer geometry is unavailable")
        {
            auto calls = systemInputCalls();
            calls.writeConsoleInput = captureReplay;
            const std::vector<core::MouseEvent> events{{.at = {5, 5}, .left = true}};
            replayedRecords.clear();
            replayWrites = 0;

            SUBCASE("the CSBI call failed")
            {
                calls.getConsoleScreenBufferInfo = unavailableConsoleInfo;
            }
            SUBCASE("the CSBI described a degenerate buffer")
            {
                calls.getConsoleScreenBufferInfo = degenerateConsoleInfo;
            }

            Input input{1, 2, calls};
            const auto outcome = input.replay(events);
            CHECK(outcome == core::ReplayOutcome{false, 1, 0});
            CHECK(core::replayOutcome(outcome) == std::unexpected(core::Error::Unavailable));
            CHECK(replayWrites == 0);
            CHECK(replayedRecords.empty());
        }

        TEST_CASE("a partial console replay is rejected by core")
        {
            auto calls = systemInputCalls();
            calls.mouseEvent = recordMouseEvent;
            calls.getConsoleScreenBufferInfo = tallConsoleInfo;
            calls.writeConsoleInput = partialWrite;
            Input input{1, calls};
            const std::vector<core::MouseEvent> events{{}, {}};
            const auto replay = input.replay(events);
            CHECK(replay == core::ReplayOutcome{true, 2, 1});
            CHECK(core::replayOutcome(replay) == std::unexpected(core::Error::Unavailable));
        }

        TEST_CASE("press and release deliver both transitions to an owned window" *
                  doctest::skip(!visibleWindowStation()))
        {
            CursorGuard cursor;
            if (!cursor.captured()) {
                std::fputs("SKIP: injected-click integration lost cursor access\n", stderr);
                return;
            }
            WindowGuard window{cursor.position()};
            if (window.get() == nullptr || !establishClickPoint(window.get(), cursor.position())) {
                std::fputs("SKIP: injected-click integration lacks an owned cursor point with both buttons up\n",
                           stderr);
                return;
            }

            clicks = {};
            Input adapter;
            adapter.press(core::Button::Left);
            adapter.release(core::Button::Left);
            adapter.press(core::Button::Right);
            adapter.release(core::Button::Right);
            pumpUntilClicksComplete();

            CHECK(clicks.leftDown == 1);
            CHECK(clicks.leftUp == 1);
            CHECK(clicks.rightDown == 1);
            CHECK(clicks.rightUp == 1);
        }

        TEST_CASE("button injection maps both buttons without using the desktop")
        {
            auto calls = systemInputCalls();
            calls.mouseEvent = recordMouseEvent;
            injectedFlags.clear();
            Input adapter{0, calls};
            adapter.press(core::Button::Left);
            adapter.release(core::Button::Left);
            adapter.press(core::Button::Right);
            adapter.release(core::Button::Right);
            CHECK(injectedFlags == std::vector<DWORD>{MOUSEEVENTF_MOVE | MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP,
                                                      MOUSEEVENTF_MOVE | MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP});

            Input systemAdapter;
            static_cast<void>(systemAdapter);
        }
    }

} // namespace burlak::adapters::win
