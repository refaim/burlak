#include "adapters/win/Focus.hpp"
#include "adapters/win/Peers.hpp"

#include <doctest/doctest.h>

#include <windows.h>

#include <optional>

namespace burlak::adapters::win
{

    namespace
    {

        std::uint64_t fakeTick{123};
        HWND fakeConsole{};
        HWND fakeOwner{};
        bool fakeVisible{true};
        bool fakeRectSucceeds{true};
        RECT fakeRect{1, 2, 101, 202};
        std::vector<UINT> allowedMessages;
        bool filterSucceeds{true};
        int filterCalls{};
        int filterFailure{};
        UINT menuCommand{};
        bool foregroundCalled{};
        int appendCalls{};
        int appendFailure{};

        ULONGLONG WINAPI tick()
        {
            return fakeTick;
        }

        HWND WINAPI consoleWindow()
        {
            return fakeConsole;
        }

        HWND WINAPI ownerWindow(HWND, UINT)
        {
            return fakeOwner;
        }

        BOOL WINAPI visible(HWND)
        {
            return fakeVisible ? TRUE : FALSE;
        }

        BOOL WINAPI windowRect(HWND, LPRECT rect)
        {
            *rect = fakeRect;
            return fakeRectSucceeds ? TRUE : FALSE;
        }

        BOOL WINAPI allowMessage(HWND, UINT message, DWORD action, PCHANGEFILTERSTRUCT)
        {
            CHECK(action == MSGFLT_ALLOW);
            allowedMessages.push_back(message);
            ++filterCalls;
            return filterSucceeds && filterCalls != filterFailure ? TRUE : FALSE;
        }

        BOOL WINAPI chooseMenu(HMENU, UINT flags, int x, int y, int, HWND, const RECT *)
        {
            CHECK((flags & TPM_RETURNCMD) != 0);
            CHECK((flags & TPM_NONOTIFY) != 0);
            CHECK((flags & TPM_RIGHTBUTTON) != 0);
            CHECK(x == 7);
            CHECK(y == 9);
            return static_cast<BOOL>(menuCommand);
        }

        BOOL WINAPI foreground(HWND)
        {
            foregroundCalled = true;
            return TRUE;
        }

        HMENU WINAPI failMenu()
        {
            return nullptr;
        }

        BOOL WINAPI controlledAppend(HMENU menu, UINT flags, UINT_PTR command, LPCWSTR text)
        {
            ++appendCalls;
            return appendCalls == appendFailure ? FALSE : AppendMenuW(menu, flags, command, text);
        }

        UINT WINAPI failRegisterMessage(LPCWSTR)
        {
            return 0;
        }

        struct Router
        {
            Peers *peers{};
            std::optional<core::PeerPayload> received;
        };

        LRESULT CALLBACK routerProcedure(HWND window, UINT message, WPARAM word, LPARAM number)
        {
            auto *router = reinterpret_cast<Router *>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (message == WM_NCCREATE) {
                const auto create = reinterpret_cast<const CREATESTRUCTW *>(number);
                router = static_cast<Router *>(create->lpCreateParams);
                SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(router));
            }
            if (router != nullptr && router->peers != nullptr) {
                if (message == router->peers->announcementMessage()) {
                    return router->peers->reply(static_cast<core::NativeWindow>(number),
                                                reinterpret_cast<core::NativeWindow>(window))
                               ? 1
                               : 0;
                }
                if (message == WM_COPYDATA) {
                    router->received = router->peers->receive(number);
                    return router->received ? 1 : 0;
                }
            }
            return DefWindowProcW(window, message, word, number);
        }

        HWND routerWindow(Router &router)
        {
            WNDCLASSW windowClass{};
            windowClass.lpfnWndProc = routerProcedure;
            windowClass.hInstance = GetModuleHandleW(nullptr);
            windowClass.lpszClassName = L"BurlakPeerAdapterTest";
            static_cast<void>(RegisterClassW(&windowClass));
            return CreateWindowExW(0, windowClass.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr,
                                   windowClass.hInstance, &router);
        }

        PeerCalls fakeHostCalls()
        {
            auto calls = systemPeerCalls();
            calls.changeFilter = allowMessage;
            calls.getTickCount = tick;
            calls.getConsoleWindow = consoleWindow;
            calls.isWindowVisible = visible;
            calls.getWindowRect = windowRect;
            calls.getWindow = ownerWindow;
            return calls;
        }

        void pumpOneMessage()
        {
            MSG message{};
            REQUIRE(PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE);
            static_cast<void>(DispatchMessageW(&message));
        }

    } // namespace

    TEST_SUITE("peer adapter")
    {
        TEST_CASE("focus records a monotonic tick through an atomic value")
        {
            const FocusCalls calls{tick};
            Focus focus{calls};
            CHECK(focus.last() == 0);
            fakeTick = 123;
            focus.record();
            CHECK(focus.last() == 123);
            CHECK(systemFocusCalls().tickCount == GetTickCount64);
        }

        TEST_CASE("two real tool windows announce, answer, and send a drop without broadcasting")
        {
            Focus firstFocus;
            Focus secondFocus;
            firstFocus.record();
            secondFocus.record();
            auto calls = fakeHostCalls();
            Router firstRouter;
            Router secondRouter;
            Peers first{firstFocus, calls};
            Peers second{secondFocus, calls};
            firstRouter.peers = &first;
            secondRouter.peers = &second;
            const HWND firstWindow = routerWindow(firstRouter);
            const HWND secondWindow = routerWindow(secondRouter);
            REQUIRE(firstWindow != nullptr);
            REQUIRE(secondWindow != nullptr);
            fakeConsole = secondWindow;
            allowedMessages.clear();
            filterSucceeds = true;
            CHECK(first.allowMessages(reinterpret_cast<core::NativeWindow>(firstWindow)));
            CHECK(first.broadcastTarget() == reinterpret_cast<core::NativeWindow>(HWND_BROADCAST));
            CHECK(allowedMessages == std::vector<UINT>{first.announcementMessage(), WM_COPYDATA});

            first.announce(reinterpret_cast<core::NativeWindow>(firstWindow),
                           reinterpret_cast<core::NativeWindow>(secondWindow));
            pumpOneMessage();
            REQUIRE(firstRouter.received.has_value());
            REQUIRE(std::holds_alternative<core::PeerHello>(*firstRouter.received));
            const auto hello = std::get<core::PeerHello>(*firstRouter.received);
            CHECK(hello.process == GetCurrentProcessId());
            CHECK(hello.tool == reinterpret_cast<core::NativeWindow>(secondWindow));
            CHECK(hello.host == reinterpret_cast<core::NativeWindow>(secondWindow));

            const core::Peer peer{.window = reinterpret_cast<core::NativeWindow>(secondWindow)};
            const core::Drop drop{.paths = {L"C:\\one.txt"}, .at = {5, 6}, .effect = core::Effect::Copy};
            CHECK(first.send(peer, drop).has_value());
            REQUIRE(secondRouter.received.has_value());
            REQUIRE(std::holds_alternative<core::Drop>(*secondRouter.received));
            CHECK(std::get<core::Drop>(*secondRouter.received) == drop);

            DestroyWindow(secondWindow);
            DestroyWindow(firstWindow);
            fakeConsole = nullptr;
        }

        TEST_CASE("host discovery prefers visible nonempty conhost and otherwise uses its owner")
        {
            const FocusCalls focusCalls{tick};
            Focus focus{focusCalls};
            auto calls = fakeHostCalls();
            Peers peers{focus, calls};
            Router router;
            router.peers = &peers;
            const HWND recipient = routerWindow(router);
            REQUIRE(recipient != nullptr);

            fakeConsole = reinterpret_cast<HWND>(10);
            fakeOwner = reinterpret_cast<HWND>(20);
            fakeVisible = true;
            fakeRectSucceeds = true;
            fakeRect = {0, 0, 100, 100};
            CHECK(peers.reply(reinterpret_cast<core::NativeWindow>(recipient), 30));
            CHECK(std::get<core::PeerHello>(*router.received).host == 10);

            fakeVisible = false;
            CHECK(peers.reply(reinterpret_cast<core::NativeWindow>(recipient), 30));
            CHECK(std::get<core::PeerHello>(*router.received).host == 20);
            fakeVisible = true;
            fakeRect = {0, 0, 0, 100};
            CHECK(peers.reply(reinterpret_cast<core::NativeWindow>(recipient), 30));
            CHECK(std::get<core::PeerHello>(*router.received).host == 20);
            fakeRect = {0, 0, 100, 0};
            CHECK(peers.reply(reinterpret_cast<core::NativeWindow>(recipient), 30));
            CHECK(std::get<core::PeerHello>(*router.received).host == 20);
            fakeRectSucceeds = false;
            CHECK(peers.reply(reinterpret_cast<core::NativeWindow>(recipient), 30));
            CHECK(std::get<core::PeerHello>(*router.received).host == 20);

            fakeConsole = reinterpret_cast<HWND>(10);
            fakeOwner = nullptr;
            CHECK_FALSE(peers.reply(reinterpret_cast<core::NativeWindow>(recipient), 30));
            fakeConsole = nullptr;
            CHECK_FALSE(peers.reply(reinterpret_cast<core::NativeWindow>(recipient), 30));
            DestroyWindow(recipient);
        }

        TEST_CASE("message filters, invalid payloads, failed sends, and menu outcomes are bounded")
        {
            const FocusCalls focusCalls{tick};
            Focus focus{focusCalls};
            auto calls = fakeHostCalls();
            Peers peers{focus, calls};
            CHECK(peers.now() == fakeTick);
            allowedMessages.clear();
            filterSucceeds = false;
            filterCalls = 0;
            filterFailure = 0;
            CHECK_FALSE(peers.allowMessages(1));

            allowedMessages.clear();
            filterSucceeds = true;
            filterCalls = 0;
            filterFailure = 2;
            CHECK_FALSE(peers.allowMessages(1));
            CHECK(allowedMessages == std::vector<UINT>{peers.announcementMessage(), WM_COPYDATA});
            filterFailure = 0;

            CHECK_FALSE(peers.receive(0).has_value());
            COPYDATASTRUCT copy{};
            CHECK_FALSE(peers.receive(reinterpret_cast<std::intptr_t>(&copy)).has_value());
            std::byte one{};
            copy = {.dwData = 999, .cbData = 0, .lpData = &one};
            CHECK_FALSE(peers.receive(reinterpret_cast<std::intptr_t>(&copy)).has_value());
            copy = {.dwData = 999, .cbData = 1, .lpData = &one};
            CHECK_FALSE(peers.receive(reinterpret_cast<std::intptr_t>(&copy)).has_value());
            copy.dwData = peerHelloDataKind();
            CHECK_FALSE(peers.receive(reinterpret_cast<std::intptr_t>(&copy)).has_value());
            copy.dwData = peerDropDataKind();
            CHECK_FALSE(peers.receive(reinterpret_cast<std::intptr_t>(&copy)).has_value());
            CHECK(peers.send(core::Peer{}, core::Drop{}) == std::unexpected(core::Error::Unavailable));
            CHECK(peers.send(core::Peer{.window = 777}, core::Drop{}) == std::unexpected(core::Error::Unavailable));

            auto noAnnouncementCalls = calls;
            noAnnouncementCalls.registerMessage = failRegisterMessage;
            Peers noAnnouncement{focus, noAnnouncementCalls};
            allowedMessages.clear();
            CHECK_FALSE(noAnnouncement.allowMessages(1));
            CHECK(allowedMessages == std::vector<UINT>{WM_COPYDATA});

            calls.trackMenu = chooseMenu;
            calls.setForegroundWindow = foreground;
            Peers menus{focus, calls};
            foregroundCalled = false;
            menuCommand = 1;
            CHECK(menus.menu(1, {7, 9}) == core::PeerMenuChoice::Copy);
            CHECK(foregroundCalled);
            menuCommand = 2;
            CHECK(menus.menu(1, {7, 9}) == core::PeerMenuChoice::Move);
            menuCommand = 0;
            CHECK(menus.menu(1, {7, 9}) == core::PeerMenuChoice::Cancel);

            calls.createMenu = failMenu;
            Peers noMenu{focus, calls};
            CHECK(noMenu.menu(1, {7, 9}) == core::PeerMenuChoice::Cancel);
            calls.createMenu = CreatePopupMenu;
            calls.appendMenu = controlledAppend;
            for (appendFailure = 1; appendFailure <= 4; ++appendFailure) {
                appendCalls = 0;
                Peers noItems{focus, calls};
                CHECK(noItems.menu(1, {7, 9}) == core::PeerMenuChoice::Cancel);
                CHECK(appendCalls == appendFailure);
            }
            CHECK(systemPeerCalls().sendMessage == SendMessageW);
            CHECK(peerHelloDataKind() != peerDropDataKind());
        }
    }

} // namespace burlak::adapters::win
