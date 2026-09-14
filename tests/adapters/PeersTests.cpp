#include "adapters/win/Focus.hpp"
#include "adapters/win/Peers.hpp"

#include <doctest/doctest.h>

#include <windows.h>

#include <cstring>
#include <optional>
#include <vector>

namespace burlak::adapters::win
{

    namespace
    {

        std::uint64_t fakeTick{123};
        std::uint64_t fakeNonce{0x1122334455667788ULL};
        NTSTATUS randomStatus{};
        HWND fakeConsole{};
        HWND fakeOwner{};
        bool fakeVisible{true};
        bool fakeRectSucceeds{true};
        RECT fakeRect{1, 2, 101, 202};
        UINT menuCommand{};
        bool foregroundCalled{};
        int appendCalls{};
        int appendFailure{};
        int timedSendCalls{};
        UINT timedSendFlags{};
        UINT timedSendTimeout{};
        bool timedSendFails{};
        bool timedSendRejects{};
        DWORD timedSendLastError{};
        int registerCalls{};

        ULONGLONG WINAPI tick()
        {
            return fakeTick;
        }

        NTSTATUS WINAPI randomBytes(BCRYPT_ALG_HANDLE, PUCHAR buffer, ULONG size, ULONG flags)
        {
            CHECK(size == sizeof(fakeNonce));
            CHECK(flags == BCRYPT_USE_SYSTEM_PREFERRED_RNG);
            std::memcpy(buffer, &fakeNonce, sizeof(fakeNonce));
            return randomStatus;
        }

        LRESULT WINAPI timedSend(HWND window, UINT message, WPARAM word, LPARAM number, UINT flags, UINT timeout,
                                 PDWORD_PTR result)
        {
            ++timedSendCalls;
            timedSendFlags = flags;
            timedSendTimeout = timeout;
            if (timedSendFails) {
                return 0;
            }
            if (timedSendRejects) {
                *result = 0;
                return 1;
            }
            return SendMessageTimeoutW(window, message, word, number, flags, timeout, result);
        }

        DWORD WINAPI sendLastError()
        {
            return timedSendLastError;
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

        UINT WINAPI partialRegisterMessage(LPCWSTR name)
        {
            ++registerCalls;
            return registerCalls == 2 ? 0 : RegisterWindowMessageW(name);
        }

        DWORD WINAPI failWindowProcess(HWND, LPDWORD process)
        {
            *process = 0;
            return 0;
        }

        DWORD WINAPI zeroWindowProcess(HWND, LPDWORD process)
        {
            *process = 0;
            return 1;
        }

        struct Router
        {
            Peers *peers{};
            std::optional<core::PeerEnvelope> received;
            std::optional<core::PeerAnnouncement> announcement;
            std::uint64_t receiverNonce{222};
            bool answer{true};
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
                if (router->peers->isAnnouncementMessage(message)) {
                    const auto announcement = router->peers->receiveAnnouncement(message, word, number);
                    if (announcement) {
                        router->announcement = announcement;
                        if (router->answer && announcement->action == core::PeerAnnouncementAction::Begin) {
                            const auto outcome = core::peerSendOutcome(
                                router->peers->reply(announcement->source, reinterpret_cast<core::NativeWindow>(window),
                                                     announcement->nonce, router->receiverNonce));
                            return outcome == core::PeerSendOutcome::Failed ? 0 : 1;
                        }
                    }
                    return 1;
                }
                if (message == WM_COPYDATA) {
                    router->received = router->peers->receive(word, number);
                    return router->received ? 1 : 0;
                }
            }
            return DefWindowProcW(window, message, word, number);
        }

        HWND routerWindow(Router &router, std::wstring_view className = L"BurlakToolWindow")
        {
            WNDCLASSW windowClass{};
            windowClass.lpfnWndProc = routerProcedure;
            windowClass.hInstance = GetModuleHandleW(nullptr);
            windowClass.lpszClassName = className.data();
            static_cast<void>(RegisterClassW(&windowClass));
            return CreateWindowExW(0, windowClass.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr,
                                   windowClass.hInstance, &router);
        }

        PeerCalls fakeHostCalls()
        {
            auto calls = systemPeerCalls();
            calls.sendMessageTimeout = timedSend;
            calls.getLastError = sendLastError;
            calls.random = randomBytes;
            calls.getConsoleWindow = consoleWindow;
            calls.isWindowVisible = visible;
            calls.getWindowRect = windowRect;
            calls.getWindow = ownerWindow;
            return calls;
        }

        void pumpMessages(std::size_t count)
        {
            for (std::size_t index = 0; index < count; ++index) {
                MSG message{};
                REQUIRE(PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE);
                static_cast<void>(DispatchMessageW(&message));
            }
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

        TEST_CASE("two real tool windows complete a nonce-bound exchange without broadcasting")
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
            const HWND firstWindow = routerWindow(firstRouter, first.toolWindowClass());
            const HWND secondWindow = routerWindow(secondRouter, second.toolWindowClass());
            REQUIRE(firstWindow != nullptr);
            REQUIRE(secondWindow != nullptr);
            fakeConsole = secondWindow;
            timedSendCalls = 0;
            timedSendFails = false;
            timedSendRejects = false;
            CHECK(first.broadcastTarget() == reinterpret_cast<core::NativeWindow>(HWND_BROADCAST));

            constexpr std::uint64_t sourceNonce = 0x8877665544332211ULL;
            first.announce(reinterpret_cast<core::NativeWindow>(firstWindow),
                           reinterpret_cast<core::NativeWindow>(secondWindow), sourceNonce);
            pumpMessages(2);
            REQUIRE(secondRouter.announcement.has_value());
            CHECK(secondRouter.announcement->nonce == sourceNonce);
            REQUIRE(firstRouter.received.has_value());
            REQUIRE(std::holds_alternative<core::PeerHello>(firstRouter.received->payload));
            const auto hello = std::get<core::PeerHello>(firstRouter.received->payload);
            CHECK(firstRouter.received->sender ==
                  core::PeerIdentity{reinterpret_cast<core::NativeWindow>(secondWindow), GetCurrentProcessId()});
            CHECK(hello.process == GetCurrentProcessId());
            CHECK(hello.tool == reinterpret_cast<core::NativeWindow>(secondWindow));
            CHECK(hello.host == reinterpret_cast<core::NativeWindow>(secondWindow));
            CHECK(hello.echoNonce == sourceNonce);
            CHECK(hello.nonce == secondRouter.receiverNonce);
            CHECK(timedSendFlags == (SMTO_ABORTIFHUNG | SMTO_BLOCK));
            CHECK(timedSendTimeout == peerSendTimeoutMilliseconds());

            const core::Peer peer{.window = reinterpret_cast<core::NativeWindow>(secondWindow),
                                  .process = GetCurrentProcessId(),
                                  .nonce = secondRouter.receiverNonce};
            const core::Drop drop{.paths = {L"C:\\one.txt"},
                                  .at = {5, 6},
                                  .effect = core::Effect::Copy,
                                  .nonce = secondRouter.receiverNonce};
            CHECK(core::peerSendOutcome(first.send(peer, drop)) == core::PeerSendOutcome::Accepted);
            REQUIRE(secondRouter.received.has_value());
            REQUIRE(std::holds_alternative<core::Drop>(secondRouter.received->payload));
            CHECK(std::get<core::Drop>(secondRouter.received->payload) == drop);

            first.endAnnouncement(reinterpret_cast<core::NativeWindow>(firstWindow),
                                  reinterpret_cast<core::NativeWindow>(secondWindow), sourceNonce);
            pumpMessages(2);
            CHECK(secondRouter.announcement->action == core::PeerAnnouncementAction::End);
            CHECK(timedSendCalls == 2);

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
            router.answer = false;
            const HWND recipient = routerWindow(router, peers.toolWindowClass());
            REQUIRE(recipient != nullptr);
            const auto tool = reinterpret_cast<core::NativeWindow>(recipient);

            fakeConsole = reinterpret_cast<HWND>(10);
            fakeOwner = reinterpret_cast<HWND>(20);
            fakeVisible = true;
            fakeRectSucceeds = true;
            fakeRect = {0, 0, 100, 100};
            CHECK(peers.reply({.window = tool, .process = GetCurrentProcessId()}, tool, 11, 22));
            CHECK(std::get<core::PeerHello>(router.received->payload).host == 10);
            const auto sendsBeforeMismatch = timedSendCalls;
            CHECK_FALSE(peers.reply({.window = tool, .process = GetCurrentProcessId() + 1}, tool, 11, 22));
            CHECK(timedSendCalls == sendsBeforeMismatch);

            fakeVisible = false;
            CHECK(peers.reply({.window = tool, .process = GetCurrentProcessId()}, tool, 11, 22));
            CHECK(std::get<core::PeerHello>(router.received->payload).host == 20);
            fakeVisible = true;
            fakeRect = {0, 0, 0, 100};
            CHECK(peers.reply({.window = tool, .process = GetCurrentProcessId()}, tool, 11, 22));
            CHECK(std::get<core::PeerHello>(router.received->payload).host == 20);
            fakeRect = {0, 0, 100, 0};
            CHECK(peers.reply({.window = tool, .process = GetCurrentProcessId()}, tool, 11, 22));
            CHECK(std::get<core::PeerHello>(router.received->payload).host == 20);
            fakeRectSucceeds = false;
            CHECK(peers.reply({.window = tool, .process = GetCurrentProcessId()}, tool, 11, 22));
            CHECK(std::get<core::PeerHello>(router.received->payload).host == 20);

            fakeConsole = reinterpret_cast<HWND>(10);
            fakeOwner = nullptr;
            CHECK_FALSE(peers.reply({.window = tool, .process = GetCurrentProcessId()}, tool, 11, 22));
            fakeConsole = nullptr;
            CHECK_FALSE(peers.reply({.window = tool, .process = GetCurrentProcessId()}, tool, 11, 22));
            DestroyWindow(recipient);
        }

        TEST_CASE("announcement assembly, random failures, identities, and timed sends reject invalid input")
        {
            Focus focus;
            auto calls = fakeHostCalls();
            Peers peers{focus, calls};
            Router router;
            router.peers = &peers;
            const HWND valid = routerWindow(router, peers.toolWindowClass());
            Router otherRouter;
            const HWND other = routerWindow(otherRouter, peers.toolWindowClass());
            Router foreignRouter;
            const HWND foreign = routerWindow(foreignRouter, L"BurlakPeerForeignTest");
            REQUIRE(valid != nullptr);
            REQUIRE(other != nullptr);
            REQUIRE(foreign != nullptr);

            randomStatus = 0;
            fakeNonce = 123;
            CHECK(peers.newNonce() == std::expected<std::uint64_t, core::Error>{123});
            fakeNonce = 0;
            CHECK(peers.newNonce() == std::unexpected(core::Error::Unavailable));
            fakeNonce = 123;
            randomStatus = static_cast<NTSTATUS>(-1);
            CHECK(peers.newNonce() == std::unexpected(core::Error::Unavailable));
            randomStatus = 0;

            CHECK_FALSE(peers.isAnnouncementMessage(0));
            CHECK_FALSE(peers.isAnnouncementMessage(WM_APP));
            CHECK_FALSE(peers.receiveAnnouncement(0, reinterpret_cast<WPARAM>(valid), 1));
            CHECK_FALSE(peers.receiveAnnouncement(WM_APP, reinterpret_cast<WPARAM>(valid), 1));
            peers.announce(reinterpret_cast<core::NativeWindow>(valid), reinterpret_cast<core::NativeWindow>(valid),
                           0x100000002ULL);
            MSG low{};
            MSG high{};
            REQUIRE(PeekMessageW(&low, nullptr, 0, 0, PM_REMOVE) != FALSE);
            REQUIRE(PeekMessageW(&high, nullptr, 0, 0, PM_REMOVE) != FALSE);
            CHECK_FALSE(peers.receiveAnnouncement(high.message, high.wParam, high.lParam));
            CHECK_FALSE(peers.receiveAnnouncement(low.message, reinterpret_cast<WPARAM>(foreign), low.lParam));
            CHECK_FALSE(peers.receiveAnnouncement(low.message, low.wParam, low.lParam));
            CHECK_FALSE(peers.receiveAnnouncement(high.message, reinterpret_cast<WPARAM>(other), high.lParam));
            CHECK_FALSE(peers.receiveAnnouncement(low.message, low.wParam, low.lParam));
            CHECK_FALSE(peers.receiveAnnouncement(high.message, reinterpret_cast<WPARAM>(foreign), high.lParam));
            CHECK_FALSE(peers.receiveAnnouncement(low.message, low.wParam, 0));
            CHECK_FALSE(peers.receiveAnnouncement(high.message, high.wParam, 0));

            CHECK_FALSE(peers.receive(0, 0));
            CHECK_FALSE(peers.receive(777, 1));
            CHECK_FALSE(peers.receive(reinterpret_cast<WPARAM>(valid), 0));
            CHECK_FALSE(peers.receive(reinterpret_cast<WPARAM>(foreign), 1));
            COPYDATASTRUCT copy{};
            CHECK_FALSE(peers.receive(reinterpret_cast<WPARAM>(valid), reinterpret_cast<std::intptr_t>(&copy)));
            std::byte one{};
            copy = {.dwData = 999, .cbData = 0, .lpData = &one};
            CHECK_FALSE(peers.receive(reinterpret_cast<WPARAM>(valid), reinterpret_cast<std::intptr_t>(&copy)));
            copy.cbData = 1;
            CHECK_FALSE(peers.receive(reinterpret_cast<WPARAM>(valid), reinterpret_cast<std::intptr_t>(&copy)));
            copy.dwData = peerHelloDataKind();
            CHECK_FALSE(peers.receive(reinterpret_cast<WPARAM>(valid), reinterpret_cast<std::intptr_t>(&copy)));
            copy.dwData = peerDropDataKind();
            CHECK_FALSE(peers.receive(reinterpret_cast<WPARAM>(valid), reinterpret_cast<std::intptr_t>(&copy)));

            const core::Drop validDrop{.paths = {L"C:\\one.txt"}, .effect = core::Effect::Copy, .nonce = 5};
            const auto process = GetCurrentProcessId();
            CHECK(peers.send(core::Peer{}, validDrop) == std::unexpected(core::Error::Unavailable));
            CHECK(peers.send(core::Peer{.window = 0, .host = 0, .lastFocus = 0, .process = 0, .nonce = 5}, validDrop) ==
                  std::unexpected(core::Error::Unavailable));
            CHECK(
                peers.send(core::Peer{.window = reinterpret_cast<core::NativeWindow>(valid), .process = 0, .nonce = 5},
                           validDrop) == std::unexpected(core::Error::Unavailable));
            CHECK(peers.send(core::Peer{.window = reinterpret_cast<core::NativeWindow>(valid), .nonce = 6},
                             validDrop) == std::unexpected(core::Error::Unavailable));
            CHECK(
                peers.send(
                    core::Peer{.window = reinterpret_cast<core::NativeWindow>(foreign), .process = process, .nonce = 5},
                    validDrop) == std::unexpected(core::Error::Unavailable));
            CHECK(peers.send(core::Peer{.window = 777, .host = 0, .lastFocus = 0, .process = process, .nonce = 5},
                             validDrop) == std::unexpected(core::Error::Unavailable));
            CHECK(peers.send(core::Peer{.window = reinterpret_cast<core::NativeWindow>(valid), .nonce = 5},
                             core::Drop{.paths = {}, .at = {}, .effect = core::Effect::Copy, .nonce = 5}) ==
                  std::unexpected(core::Error::Unavailable));
            CHECK(peers.send(core::Peer{.window = reinterpret_cast<core::NativeWindow>(valid),
                                        .process = process + 1,
                                        .nonce = 5},
                             validDrop) == std::unexpected(core::Error::Unavailable));

            peers.announce(reinterpret_cast<core::NativeWindow>(valid), reinterpret_cast<core::NativeWindow>(valid), 5);
            pumpMessages(2);
            timedSendFails = true;
            timedSendLastError = ERROR_TIMEOUT;
            const auto timedOut = peers.send(
                core::Peer{.window = reinterpret_cast<core::NativeWindow>(valid), .process = process, .nonce = 5},
                validDrop);
            REQUIRE(timedOut.has_value());
            CHECK(*timedOut ==
                  core::PeerTransportResult{
                      .sent = 0, .receiver = 0, .lastError = ERROR_TIMEOUT, .timeoutError = ERROR_TIMEOUT});
            CHECK(core::peerSendOutcome(timedOut) == core::PeerSendOutcome::Indeterminate);
            timedSendLastError = ERROR_INVALID_WINDOW_HANDLE;
            const auto failed = peers.send(
                core::Peer{.window = reinterpret_cast<core::NativeWindow>(valid), .process = process, .nonce = 5},
                validDrop);
            REQUIRE(failed.has_value());
            CHECK(failed->lastError == ERROR_INVALID_WINDOW_HANDLE);
            CHECK(core::peerSendOutcome(failed) == core::PeerSendOutcome::Failed);
            timedSendFails = false;
            timedSendRejects = true;
            CHECK(core::peerSendOutcome(peers.send(
                      core::Peer{.window = reinterpret_cast<core::NativeWindow>(valid), .process = process, .nonce = 5},
                      validDrop)) == core::PeerSendOutcome::Failed);
            timedSendRejects = false;

            auto noProcessCalls = calls;
            noProcessCalls.getWindowProcess = failWindowProcess;
            Peers noProcess{focus, noProcessCalls};
            CHECK_FALSE(noProcess.receive(reinterpret_cast<WPARAM>(valid), 1));
            CHECK(noProcess.send(
                      core::Peer{.window = reinterpret_cast<core::NativeWindow>(valid), .process = process, .nonce = 5},
                      validDrop) == std::unexpected(core::Error::Unavailable));
            noProcessCalls.getWindowProcess = zeroWindowProcess;
            Peers zeroProcess{focus, noProcessCalls};
            CHECK_FALSE(zeroProcess.receive(reinterpret_cast<WPARAM>(valid), 1));
            CHECK(zeroProcess.send(
                      core::Peer{.window = reinterpret_cast<core::NativeWindow>(valid), .process = process, .nonce = 5},
                      validDrop) == std::unexpected(core::Error::Unavailable));

            peers.announce(0, reinterpret_cast<core::NativeWindow>(valid), 5);
            peers.announce(reinterpret_cast<core::NativeWindow>(valid), 0, 5);
            peers.announce(reinterpret_cast<core::NativeWindow>(valid), reinterpret_cast<core::NativeWindow>(valid), 0);

            auto noMessagesCalls = calls;
            noMessagesCalls.registerMessage = failRegisterMessage;
            Peers noMessages{focus, noMessagesCalls};
            noMessages.announce(reinterpret_cast<core::NativeWindow>(valid),
                                reinterpret_cast<core::NativeWindow>(valid), 5);
            noMessages.endAnnouncement(reinterpret_cast<core::NativeWindow>(valid),
                                       reinterpret_cast<core::NativeWindow>(valid), 5);
            CHECK_FALSE(noMessages.isAnnouncementMessage(0));
            auto partialMessagesCalls = calls;
            registerCalls = 0;
            partialMessagesCalls.registerMessage = partialRegisterMessage;
            Peers partialMessages{focus, partialMessagesCalls};
            partialMessages.announce(reinterpret_cast<core::NativeWindow>(valid),
                                     reinterpret_cast<core::NativeWindow>(valid), 5);

            DestroyWindow(foreign);
            DestroyWindow(other);
            DestroyWindow(valid);
        }

        TEST_CASE("popup menu returns copy, move, or cancel and contains construction failures")
        {
            Focus focus;
            auto calls = fakeHostCalls();
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
            CHECK(systemPeerCalls().sendMessageTimeout == SendMessageTimeoutW);
            CHECK(systemPeerCalls().random == BCryptGenRandom);
            CHECK(peerHelloDataKind() != peerDropDataKind());
        }
    }

} // namespace burlak::adapters::win
