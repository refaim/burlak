#include "adapters/win/DropMenu.hpp"
#include "adapters/win/WindowProperties.hpp"

#include "../Desktop.hpp"

#include <doctest/doctest.h>

#include <windows.h>

#include <string>
#include <vector>

namespace burlak::adapters::win
{

    namespace
    {

        UINT selectedCommand{};
        int appendedItems{};
        UINT copyFlags{};
        UINT moveFlags{};
        std::vector<std::string> menuTrace;
        BOOL foregroundResult{TRUE};

        HMENU WINAPI fakeCreateMenu()
        {
            return reinterpret_cast<HMENU>(1);
        }

        BOOL WINAPI fakeAppendMenu(HMENU, UINT flags, UINT_PTR command, LPCWSTR)
        {
            ++appendedItems;
            if (command == 1) {
                copyFlags = flags;
            } else if (command == 2) {
                moveFlags = flags;
            }
            return TRUE;
        }

        BOOL WINAPI fakeTrackMenu(HMENU, UINT, int, int, int, HWND, const RECT *)
        {
            menuTrace.emplace_back("track");
            return static_cast<BOOL>(selectedCommand);
        }

        BOOL WINAPI fakeDestroyMenu(HMENU)
        {
            return TRUE;
        }

        BOOL WINAPI fakeForeground(HWND)
        {
            menuTrace.emplace_back("foreground");
            return foregroundResult;
        }

        HWND WINAPI fakeGetForeground()
        {
            return reinterpret_cast<HWND>(0x2000);
        }

        DWORD WINAPI fakeGetWindowThread(HWND window, LPDWORD process)
        {
            if (process != nullptr) {
                *process = 0;
            }
            return window == reinterpret_cast<HWND>(0x2000) ? 0xF00DU : 0U;
        }

        DWORD WINAPI fakeGetCurrentThreadId()
        {
            return 0xB00BU;
        }

        BOOL WINAPI fakeAttachInput(DWORD ours, DWORD other, BOOL attach)
        {
            CHECK(ours == 0xB00BU);
            CHECK(other == 0xF00DU);
            menuTrace.emplace_back(attach != FALSE ? "attach" : "detach");
            return TRUE;
        }

        BOOL WINAPI fakePostMessage(HWND, UINT message, WPARAM, LPARAM)
        {
            if (message == WM_NULL) {
                menuTrace.emplace_back("wmnull");
            }
            return TRUE;
        }

        DropMenuCalls fakeMenuCalls()
        {
            return {fakeCreateMenu,    fakeAppendMenu,      fakeTrackMenu,          fakeDestroyMenu, fakeForeground,
                    fakeGetForeground, fakeGetWindowThread, fakeGetCurrentThreadId, fakeAttachInput, fakePostMessage};
        }

        // Drives a real TrackPopupMenu loop from the calling thread's own timers: the modal menu loop dispatches
        // thread timers, and menu keyboard navigation is read from that thread's queue, so posted keys select an item.
        struct MenuDriver
        {
            HWND owner{};
            UINT_PTR select{};
            UINT_PTR safety{};
            bool selected{};
            bool ended{};
        } menuDriver;

        void CALLBACK selectFirstItem(HWND, UINT, UINT_PTR id, DWORD)
        {
            static_cast<void>(KillTimer(nullptr, id));
            menuDriver.selected = true;
            static_cast<void>(PostMessageW(menuDriver.owner, WM_KEYDOWN, VK_DOWN, 0));
            static_cast<void>(PostMessageW(menuDriver.owner, WM_KEYDOWN, VK_RETURN, 0));
        }

        void CALLBACK endActiveMenu(HWND, UINT, UINT_PTR id, DWORD)
        {
            static_cast<void>(KillTimer(nullptr, id));
            menuDriver.ended = true;
            static_cast<void>(EndMenu());
        }

        HWND overlayLikeWindow()
        {
            return CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"", WS_POPUP, 0, 0,
                                   1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        }

        core::DropMenuChoice chooseWithDriver(DropMenu &menu, HWND owner)
        {
            menuDriver = {};
            menuDriver.owner = owner;
            menuDriver.select = SetTimer(nullptr, 0, 50, selectFirstItem);
            menuDriver.safety = SetTimer(nullptr, 0, 3000, endActiveMenu);
            const auto chosen = menu.choose(reinterpret_cast<core::NativeWindow>(owner), {0, 0});
            static_cast<void>(KillTimer(nullptr, menuDriver.select));
            static_cast<void>(KillTimer(nullptr, menuDriver.safety));
            return chosen;
        }

        struct ForeignWindowThread
        {
            HANDLE ready{};
            HANDLE stop{};
            HWND window{};
        } foreignThread;

        DWORD WINAPI foreignWindowMain(void *)
        {
            foreignThread.window = overlayLikeWindow();
            static_cast<void>(SetEvent(foreignThread.ready));
            static_cast<void>(WaitForSingleObject(foreignThread.stop, INFINITE));
            static_cast<void>(DestroyWindow(foreignThread.window));
            return 0;
        }

    } // namespace

    TEST_SUITE("window adapters")
    {
        TEST_CASE("receiver properties preserve a process value on a real window")
        {
            const HWND window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 1, 1, HWND_MESSAGE, nullptr,
                                                GetModuleHandleW(nullptr), nullptr);
            REQUIRE(window != nullptr);
            WindowProperties properties;
            const auto native = reinterpret_cast<core::NativeWindow>(window);

            CHECK_FALSE(properties.value(native).has_value());
            properties.set(native, 7);
            CHECK(properties.value(native) == 7);
            properties.set(native, 99);
            CHECK(properties.value(native) == 99);
            properties.remove(native);
            CHECK_FALSE(properties.value(native).has_value());
            CHECK(properties.processId() == GetCurrentProcessId());
            CHECK(systemWindowPropertyCalls().getProcessId == GetCurrentProcessId);
            WindowProperties injected{systemWindowPropertyCalls()};
            CHECK(injected.processId() == GetCurrentProcessId());

            DestroyWindow(window);
        }

        TEST_CASE("drop popup maps copy, move, cancel, and construction failure")
        {
            const DropMenuCalls calls = fakeMenuCalls();
            DropMenu menu{calls};
            appendedItems = 0;

            selectedCommand = 1;
            CHECK(menu.choose(10, {20, 30}) == core::DropMenuChoice::Copy);
            selectedCommand = 2;
            CHECK(menu.choose(10, {20, 30}) == core::DropMenuChoice::Move);
            selectedCommand = 0;
            CHECK(menu.choose(10, {20, 30}) == core::DropMenuChoice::Cancel);
            selectedCommand = 99;
            CHECK(menu.choose(10, {20, 30}) == core::DropMenuChoice::Cancel);
            CHECK(appendedItems == 16);

            selectedCommand = 2;
            CHECK(menu.choose(10, {20, 30}, {.move = true}) == core::DropMenuChoice::Move);
            CHECK((copyFlags & MF_GRAYED) != 0);
            CHECK((moveFlags & MF_GRAYED) == 0);
            selectedCommand = 1;
            CHECK(menu.choose(10, {20, 30}, {.copy = true}) == core::DropMenuChoice::Copy);
            CHECK((copyFlags & MF_GRAYED) == 0);
            CHECK((moveFlags & MF_GRAYED) != 0);

            auto failing = calls;
            failing.createMenu = []() -> HMENU { return nullptr; };
            CHECK(DropMenu{failing}.choose(10, {20, 30}) == core::DropMenuChoice::Cancel);
            DropMenu defaultMenu;
            static_cast<void>(defaultMenu);
            CHECK(systemDropMenuCalls().createMenu == CreatePopupMenu);
            CHECK(systemDropMenuCalls().attachInput == AttachThreadInput);
            CHECK(systemDropMenuCalls().postMessage == PostMessageW);
        }

        TEST_CASE("the popup attaches to the foreground thread around the track and posts WM_NULL after")
        {
            DropMenu menu{fakeMenuCalls()};

            menuTrace.clear();
            selectedCommand = 1;
            foregroundResult = TRUE;
            CHECK(menu.choose(10, {20, 30}) == core::DropMenuChoice::Copy);
            // attach the foreground input, set foreground and track inside it, then post WM_NULL and detach.
            CHECK(menuTrace == std::vector<std::string>{"attach", "foreground", "track", "wmnull", "detach"});

            // Even when SetForegroundWindow is refused the menu is still shown, and the attach is still balanced.
            menuTrace.clear();
            foregroundResult = FALSE;
            selectedCommand = 0;
            CHECK(menu.choose(10, {20, 30}) == core::DropMenuChoice::Cancel);
            CHECK(menuTrace == std::vector<std::string>{"attach", "foreground", "track", "wmnull", "detach"});
            foregroundResult = TRUE;
        }

        TEST_CASE("a real popup menu answers only for an owner window of the calling thread" *
                  doctest::skip(!burlak::tests::desktopAvailable(
                      "SKIP: popup menu integration requires a visible window station\n")))
        {
            DropMenu menu;

            const HWND local = overlayLikeWindow();
            REQUIRE(local != nullptr);
            CHECK(chooseWithDriver(menu, local) == core::DropMenuChoice::Copy);
            CHECK(menuDriver.selected);
            CHECK_FALSE(menuDriver.ended);
            static_cast<void>(DestroyWindow(local));

            // A host window belongs to conhost or Windows Terminal; a window of another thread in this process
            // exercises the same TrackPopupMenu refusal (ERROR_INVALID_PARAMETER before its loop ever runs).
            foreignThread = {};
            foreignThread.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            foreignThread.stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            REQUIRE(foreignThread.ready != nullptr);
            REQUIRE(foreignThread.stop != nullptr);
            const HANDLE thread = CreateThread(nullptr, 0, foreignWindowMain, nullptr, 0, nullptr);
            REQUIRE(thread != nullptr);
            REQUIRE(WaitForSingleObject(foreignThread.ready, 5000) == WAIT_OBJECT_0);
            REQUIRE(foreignThread.window != nullptr);
            CHECK(chooseWithDriver(menu, foreignThread.window) == core::DropMenuChoice::Cancel);
            CHECK_FALSE(menuDriver.selected);
            CHECK_FALSE(menuDriver.ended);
            static_cast<void>(SetEvent(foreignThread.stop));
            static_cast<void>(WaitForSingleObject(thread, 5000));
            static_cast<void>(CloseHandle(thread));
            static_cast<void>(CloseHandle(foreignThread.ready));
            static_cast<void>(CloseHandle(foreignThread.stop));
        }
    }

} // namespace burlak::adapters::win
