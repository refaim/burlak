#include "adapters/win/Screen.hpp"

#include "../Desktop.hpp"

#include <doctest/doctest.h>

#include <windows.h>

namespace burlak::adapters::win
{

    namespace
    {

        struct FakeScreenState
        {
            POINT cursor{50, 50};
            bool cursorSucceeds{true};
            SHORT leftKey{};
            SHORT rightKey{};
            HWND console{reinterpret_cast<HWND>(1)};
            HWND foreground{reinterpret_cast<HWND>(2)};
            HWND pointWindow{reinterpret_cast<HWND>(4)};
            HWND rootWindow{reinterpret_cast<HWND>(5)};
            bool rectSucceeds{true};
            bool visible{true};
            HWND hiddenWindow{};
            RECT rect{0, 0, 100, 100};
            bool clientSucceeds{true};
            RECT client{0, 0, 100, 100};
            LONG_PTR style{};
            HANDLE output{reinterpret_cast<HANDLE>(3)};
            bool fontSucceeds{true};
            COORD fontSize{10, 20};
            POINT origin{7, 9};
            HWND clientToScreenWindow{};
            bool clientToScreenSucceeds{true};
            bool bufferSucceeds{true};
            SMALL_RECT bufferWindow{0, 0, 9, 4};
        };

        FakeScreenState fakeState;

        BOOL WINAPI fakeGetCursorPos(LPPOINT point)
        {
            *point = fakeState.cursor;
            return fakeState.cursorSucceeds ? TRUE : FALSE;
        }

        SHORT WINAPI fakeGetAsyncKeyState(int key)
        {
            return key == VK_LBUTTON ? fakeState.leftKey : fakeState.rightKey;
        }

        HWND WINAPI fakeGetConsoleWindow()
        {
            return fakeState.console;
        }

        HWND WINAPI fakeGetWindow(HWND, UINT command)
        {
            CHECK(command == GW_OWNER);
            return fakeState.foreground;
        }

        HWND WINAPI fakeWindowFromPoint(POINT)
        {
            return fakeState.pointWindow;
        }

        HWND WINAPI fakeGetAncestor(HWND, UINT flag)
        {
            CHECK(flag == GA_ROOT);
            return fakeState.rootWindow;
        }

        BOOL WINAPI fakeGetWindowRect(HWND, LPRECT rect)
        {
            *rect = fakeState.rect;
            return fakeState.rectSucceeds ? TRUE : FALSE;
        }

        BOOL WINAPI fakeGetClientRect(HWND, LPRECT rect)
        {
            *rect = fakeState.client;
            return fakeState.clientSucceeds ? TRUE : FALSE;
        }

        BOOL WINAPI fakeIsWindowVisible(HWND window)
        {
            return fakeState.visible && window != fakeState.hiddenWindow ? TRUE : FALSE;
        }

        LONG_PTR WINAPI fakeGetWindowLong(HWND, int)
        {
            return fakeState.style;
        }

        HANDLE WINAPI fakeGetStdHandle(DWORD)
        {
            return fakeState.output;
        }

        BOOL WINAPI fakeGetCurrentConsoleFont(HANDLE, BOOL, PCONSOLE_FONT_INFO font)
        {
            font->dwFontSize = fakeState.fontSize;
            return fakeState.fontSucceeds ? TRUE : FALSE;
        }

        BOOL WINAPI fakeClientToScreen(HWND window, LPPOINT point)
        {
            fakeState.clientToScreenWindow = window;
            *point = fakeState.origin;
            return fakeState.clientToScreenSucceeds ? TRUE : FALSE;
        }

        BOOL WINAPI fakeGetConsoleScreenBufferInfo(HANDLE, PCONSOLE_SCREEN_BUFFER_INFO buffer)
        {
            buffer->srWindow = fakeState.bufferWindow;
            return fakeState.bufferSucceeds ? TRUE : FALSE;
        }

        const ScreenCalls fakeCalls{fakeGetCursorPos,     fakeGetAsyncKeyState,
                                    fakeGetConsoleWindow, fakeGetWindow,
                                    fakeWindowFromPoint,  fakeGetAncestor,
                                    fakeGetWindowRect,    fakeGetClientRect,
                                    fakeIsWindowVisible,  fakeGetWindowLong,
                                    fakeGetStdHandle,     fakeGetCurrentConsoleFont,
                                    fakeClientToScreen,   fakeGetConsoleScreenBufferInfo};

        void resetFakeScreen()
        {
            fakeState = {};
        }

        LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM word, LPARAM number)
        {
            return DefWindowProcW(window, message, word, number);
        }

        HWND testWindow()
        {
            WNDCLASSW windowClass{};
            windowClass.lpfnWndProc = windowProc;
            windowClass.hInstance = GetModuleHandleW(nullptr);
            windowClass.lpszClassName = L"BurlakScreenAdapterTest";
            static_cast<void>(RegisterClassW(&windowClass));
            return CreateWindowExW(0, windowClass.lpszClassName, L"", WS_POPUP | WS_VISIBLE, 100, 100, 200, 200,
                                   nullptr, nullptr, windowClass.hInstance, nullptr);
        }

    } // namespace

    TEST_SUITE("screen adapter")
    {
        TEST_CASE("a visible window covering the point is returned with its geometry and topmost state" *
                  doctest::skip(!burlak::tests::desktopAvailable(
                      "SKIP: screen integration requires a visible window station with cursor access\n")))
        {
            const HWND window = testWindow();
            REQUIRE(window != nullptr);

            const auto found = hostWindowAt({150, 150}, reinterpret_cast<core::NativeWindow>(window), 0);
            REQUIRE(found.has_value());
            CHECK(found->handle == reinterpret_cast<core::NativeWindow>(window));
            CHECK(found->rect == core::PixelRect{100, 100, 300, 300});
            CHECK_FALSE(found->topmost);

            CHECK_FALSE(hostWindowAt({50, 50}, reinterpret_cast<core::NativeWindow>(window), 0).has_value());

            ShowWindow(window, SW_HIDE);
            CHECK_FALSE(hostWindowAt({150, 150}, reinterpret_cast<core::NativeWindow>(window), 0).has_value());
            DestroyWindow(window);
        }

        TEST_CASE("the console owner is used when the console candidate does not cover the cursor" *
                  doctest::skip(!burlak::tests::desktopAvailable(
                      "SKIP: screen integration requires a visible window station with cursor access\n")))
        {
            const HWND window = testWindow();
            REQUIRE(window != nullptr);
            const auto found = hostWindowAt({150, 150}, 0, reinterpret_cast<core::NativeWindow>(window));
            REQUIRE(found.has_value());
            CHECK(found->handle == reinterpret_cast<core::NativeWindow>(window));
            DestroyWindow(window);
        }

        TEST_CASE("the real adapter exposes cursor, button, host, and console geometry as expected values")
        {
            Screen screen;
            const auto before = screen.cursor();
            static_cast<void>(screen.buttonDown(core::Button::Left));
            static_cast<void>(screen.hostWindow());
            const auto geometry = screen.cellGeometry();
            const bool expectedGeometry = geometry.has_value() || geometry.error() == core::Error::Unavailable;
            CHECK(expectedGeometry);
            static_cast<void>(before);
        }

        TEST_CASE("the injected adapter maps screen queries without relying on the desktop")
        {
            CHECK_FALSE(hostWindowAt({0, 0}, 0, 0).has_value());
            resetFakeScreen();
            Screen screen{fakeCalls};
            CHECK(screen.cursor() == std::optional{core::Point{50, 50}});
            CHECK(screen.windowAt({50, 50}) == reinterpret_cast<core::NativeWindow>(fakeState.rootWindow));
            CHECK(screen.consoleWindow() == reinterpret_cast<core::NativeWindow>(fakeState.console));
            CHECK(screen.hostWindowHandle() == reinterpret_cast<core::NativeWindow>(fakeState.console));
            fakeState.pointWindow = nullptr;
            CHECK(screen.windowAt({50, 50}) == 0);
            fakeState.pointWindow = reinterpret_cast<HWND>(4);
            CHECK_FALSE(screen.buttonDown(core::Button::Left));
            fakeState.rightKey = static_cast<SHORT>(0x8000);
            CHECK(screen.buttonDown(core::Button::Right));

            fakeState.style = WS_EX_TOPMOST;
            const auto host = screen.hostWindow();
            REQUIRE(host.has_value());
            CHECK(host->handle == reinterpret_cast<core::NativeWindow>(fakeState.console));
            CHECK(host->topmost);
            const auto pointHost = screen.hostWindowAt({50, 50});
            REQUIRE(pointHost.has_value());
            CHECK(pointHost->handle == host->handle);
            CHECK(pointHost->rect == host->rect);
            CHECK(pointHost->topmost == host->topmost);

            fakeState.rectSucceeds = false;
            CHECK_FALSE(screen.hostWindow().has_value());
            fakeState.rectSucceeds = true;
            fakeState.visible = false;
            CHECK_FALSE(screen.hostWindow().has_value());
            CHECK(screen.hostWindowHandle() == reinterpret_cast<core::NativeWindow>(fakeState.foreground));
            fakeState.visible = true;
            fakeState.rectSucceeds = false;
            CHECK(screen.hostWindowHandle() == reinterpret_cast<core::NativeWindow>(fakeState.foreground));
            fakeState.rectSucceeds = true;
            fakeState.rect.right = fakeState.rect.left;
            CHECK(screen.hostWindowHandle() == reinterpret_cast<core::NativeWindow>(fakeState.foreground));
            fakeState.rect = {0, 0, 100, 0};
            CHECK(screen.hostWindowHandle() == reinterpret_cast<core::NativeWindow>(fakeState.foreground));
            fakeState.rect = {0, 0, 100, 100};
            fakeState.visible = true;
            fakeState.cursor = {200, 200};
            CHECK_FALSE(screen.hostWindow().has_value());

            fakeState.cursor = {50, 50};
            fakeState.console = nullptr;
            CHECK(screen.consoleWindow() == 0);
            CHECK(screen.hostWindowHandle() == 0);
            CHECK_FALSE(screen.hostWindow().has_value());

            fakeState.cursorSucceeds = false;
            CHECK_FALSE(screen.cursor().has_value());
            CHECK_FALSE(screen.hostWindow().has_value());
            CHECK_FALSE(screen.hostWindowAt({50, 50}).has_value());
        }

        TEST_CASE("font metrics are preferred and the buffer geometry is a checked fallback")
        {
            resetFakeScreen();
            Screen screen{fakeCalls};
            CHECK(screen.cellGeometry() == core::CellGeometry{{7, 9}, 10, 20});
            CHECK(screen.cellGeometryAt({50, 50}) == core::CellGeometry{{7, 9}, 10, 20});
            CHECK(fakeState.clientToScreenWindow == fakeState.console);

            resetFakeScreen();
            fakeState.hiddenWindow = fakeState.console;
            fakeState.client = {0, 0, 200, 100};
            CHECK(screen.cellGeometry() == core::CellGeometry{{7, 9}, 20, 20});
            CHECK(fakeState.clientToScreenWindow == fakeState.foreground);

            fakeState.output = INVALID_HANDLE_VALUE;
            CHECK(screen.cellGeometry() == std::unexpected(core::Error::Unavailable));

            resetFakeScreen();
            fakeState.fontSucceeds = false;
            fakeState.bufferSucceeds = false;
            CHECK(screen.cellGeometry() == std::unexpected(core::Error::Unavailable));

            fakeState.bufferSucceeds = true;
            fakeState.console = nullptr;
            fakeState.foreground = nullptr;
            CHECK(screen.cellGeometry() == std::unexpected(core::Error::Unavailable));

            resetFakeScreen();
            fakeState.fontSize.X = 0;
            CHECK(screen.cellGeometry() == core::CellGeometry{{7, 9}, 10, 20});
            fakeState.fontSize = {10, 0};
            CHECK(screen.cellGeometry() == core::CellGeometry{{7, 9}, 10, 20});

            fakeState.bufferWindow = {0, 0, -1, 4};
            CHECK(screen.cellGeometry() == std::unexpected(core::Error::Unavailable));
            fakeState.bufferWindow = {0, 0, 9, -1};
            CHECK(screen.cellGeometry() == std::unexpected(core::Error::Unavailable));
            fakeState.bufferWindow = {0, 0, 9, 4};
            fakeState.client = {0, 0, 5, 100};
            CHECK(screen.cellGeometry() == std::unexpected(core::Error::Unavailable));
            fakeState.client = {0, 0, 100, 2};
            CHECK(screen.cellGeometry() == std::unexpected(core::Error::Unavailable));

            fakeState.client = {0, 0, 100, 100};
            fakeState.clientSucceeds = false;
            CHECK(screen.cellGeometry() == std::unexpected(core::Error::Unavailable));
            fakeState.clientSucceeds = true;
            fakeState.clientToScreenSucceeds = false;
            CHECK(screen.cellGeometry() == std::unexpected(core::Error::Unavailable));

            resetFakeScreen();
            fakeState.clientToScreenSucceeds = false;
            CHECK(screen.cellGeometry() == std::unexpected(core::Error::Unavailable));

            resetFakeScreen();
            fakeState.cursorSucceeds = false;
            CHECK(screen.cellGeometry() == std::unexpected(core::Error::Unavailable));
        }
    }

} // namespace burlak::adapters::win
