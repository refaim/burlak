#include "adapters/win/Input.hpp"

#include "core/Policies.hpp"

#include <doctest/doctest.h>

#include <windows.h>

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

        void WINAPI recordMouseEvent(DWORD flags, DWORD, DWORD, DWORD, ULONG_PTR)
        {
            injectedFlags.push_back(flags);
        }

        BOOL WINAPI partialWrite(HANDLE, const INPUT_RECORD *, DWORD requested, LPDWORD written)
        {
            *written = requested - 1;
            return TRUE;
        }

        bool visibleWindowStation()
        {
            USEROBJECTFLAGS flags{};
            DWORD needed{};
            POINT cursor{};
            const bool visible = GetUserObjectInformationW(GetProcessWindowStation(), UOI_FLAGS, &flags, sizeof(flags),
                                                           &needed) != FALSE &&
                                 (flags.dwFlags & WSF_VISIBLE) != 0 && GetCursorPos(&cursor) != FALSE;
            if (!visible) {
                std::fputs("SKIP: injected-click integration requires a visible window station with cursor access\n",
                           stderr);
            }
            return visible;
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

        bool ownsClickPoint(HWND window, POINT requested)
        {
            POINT positioned{};
            return SetCursorPos(requested.x, requested.y) != FALSE && GetCursorPos(&positioned) != FALSE &&
                   WindowFromPoint(positioned) == window && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0 &&
                   (GetAsyncKeyState(VK_RBUTTON) & 0x8000) == 0;
        }

        void pumpUntilFourClicks()
        {
            for (int attempt = 0;
                 attempt < 20 && (clicks.leftDown + clicks.leftUp + clicks.rightDown + clicks.rightUp) < 4; ++attempt) {
                static_cast<void>(MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT));
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
            FlushConsoleInputBuffer(consoleInput);

            Input adapter{reinterpret_cast<core::NativeWindow>(consoleInput)};
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

        TEST_CASE("a partial console replay is rejected by core")
        {
            InputCalls calls{recordMouseEvent, partialWrite};
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
            if (window.get() == nullptr || !ownsClickPoint(window.get(), cursor.position())) {
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
            pumpUntilFourClicks();

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
