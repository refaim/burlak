#include "drag/ToolWindow.hpp"

#include "../Desktop.hpp"

#include <doctest/doctest.h>

#include <windows.h>

#include <cstdint>
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
            std::optional<core::Point> point;
            core::NativeWindow root{};

            [[nodiscard]] std::optional<core::Point> cursor() override
            {
                return point;
            }

            [[nodiscard]] bool buttonDown(core::Button) override
            {
                return down;
            }

            [[nodiscard]] core::NativeWindow windowAt(core::Point) override
            {
                return root;
            }

            [[nodiscard]] std::optional<core::HostWindow> hostWindow() override
            {
                return host;
            }

            [[nodiscard]] std::optional<core::HostWindow> hostWindowAt(core::Point) override
            {
                return host;
            }

            [[nodiscard]] std::expected<core::CellGeometry, core::Error> cellGeometry() override
            {
                return std::unexpected(core::Error::Unavailable);
            }

            [[nodiscard]] std::expected<core::CellGeometry, core::Error> cellGeometryAt(core::Point) override
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
            std::size_t parsedPaths{1};
            int dragCalls{};
            bool allowedLink{};
            std::uintptr_t sourceHandle{};
            core::DragLoopOutcome dragOutcome{};
            std::optional<core::Effect> preferredEffect;
            std::function<void(core::NativeWindow)> duringDrag;

            [[nodiscard]] std::expected<PreparedDrag, core::Error> makeDataObject(
                std::span<const std::wstring>, std::optional<core::Effect> preferred) override
            {
                preferredEffect = preferred;
                if (!prepares) {
                    return std::unexpected(core::Error::NoSelection);
                }
                return PreparedDrag{.data = std::make_unique<Data>(), .parsedPaths = parsedPaths};
            }

            [[nodiscard]] core::DragLoopOutcome runDrag(core::NativeWindow owner, DragData &, std::uintptr_t source,
                                                        bool allowLink) override
            {
                ++dragCalls;
                allowedLink = allowLink;
                sourceHandle = source;
                if (duringDrag) {
                    duringDrag(owner);
                }
                return dragOutcome;
            }

            [[nodiscard]] std::expected<void, core::Error> copy(std::span<const std::wstring>, std::wstring_view,
                                                                core::Effect, core::NativeWindow) override
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
            std::vector<core::PendingPeerDrop> peerDrops;
            bool accepts{true};

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

            [[nodiscard]] bool receivePeerDrop(core::PendingPeerDrop drop) override
            {
                peerDrops.push_back(std::move(drop));
                return accepts;
            }
        };

        class Extraction final : public core::IExtraction
        {
          public:
            int calls{};
            int cleanups{};
            int retains{};
            bool succeeds{true};
            bool touched{};
            std::function<void()> duringRetain;

            [[nodiscard]] bool extract() override
            {
                ++calls;
                return succeeds;
            }

            void cleanup() override
            {
                ++cleanups;
            }

            void retain() override
            {
                if (duringRetain) {
                    duringRetain();
                }
                ++retains;
                touched = true;
            }
        };

        struct PrivateMessageProbe
        {
            Input *input{};
            Extraction *extraction{};
            std::vector<LRESULT> hostileResults;
            std::optional<std::size_t> startPressesBeforeLegitimate;
            std::optional<int> abortCleanupsBeforeLegitimate;
        } privateMessageProbe;

        LRESULT WINAPI sendPrivateWithHostileParameters(HWND window, UINT message, WPARAM word, LPARAM number)
        {
            privateMessageProbe.hostileResults.push_back(SendMessageW(window, message, 1, 0));
            privateMessageProbe.hostileResults.push_back(SendMessageW(window, message, 0, 1));
            if (message == WM_USER + 0x102) {
                privateMessageProbe.startPressesBeforeLegitimate = privateMessageProbe.input->presses.size();
            }
            if (message == WM_USER + 0x103) {
                privateMessageProbe.abortCleanupsBeforeLegitimate = privateMessageProbe.extraction->cleanups;
            }
            return SendMessageW(window, message, word, number);
        }

        class Peers final : public core::IPeers
        {
          public:
            int announcements{};
            int endings{};
            int replies{};
            int sends{};
            core::NativeWindow source{};
            core::NativeWindow target{};
            std::uint64_t announcedNonce{};
            std::expected<std::uint64_t, core::Error> nonce{100};
            std::optional<core::PeerAnnouncement> announcement;
            std::optional<core::PeerEnvelope> payload;
            core::PeerMenuChoice choice{core::PeerMenuChoice::Cancel};
            std::expected<core::PeerTransportResult, core::Error> replyResult{
                core::PeerTransportResult{.sent = 1, .receiver = 1}};
            bool throwAllocation{};

            [[nodiscard]] std::wstring_view toolWindowClass() const override
            {
                return L"BurlakToolWindow";
            }
            [[nodiscard]] bool isAnnouncementMessage(std::uint32_t message) const override
            {
                return message == WM_APP + 77;
            }
            [[nodiscard]] std::optional<core::PeerAnnouncement> receiveAnnouncement(std::uint32_t, std::uintptr_t,
                                                                                    std::intptr_t) override
            {
                return std::exchange(announcement, std::nullopt);
            }
            [[nodiscard]] std::expected<std::uint64_t, core::Error> newNonce() const override
            {
                return nonce;
            }
            [[nodiscard]] std::uint32_t processId() const override
            {
                return 10;
            }
            [[nodiscard]] core::NativeWindow broadcastTarget() const override
            {
                return 99;
            }
            void announce(core::NativeWindow announcedSource, core::NativeWindow announcedTarget,
                          std::uint64_t value) override
            {
                ++announcements;
                source = announcedSource;
                target = announcedTarget;
                announcedNonce = value;
            }
            void endAnnouncement(core::NativeWindow, core::NativeWindow, std::uint64_t) override
            {
                ++endings;
            }
            [[nodiscard]] std::expected<core::PeerTransportResult, core::Error> reply(core::PeerIdentity,
                                                                                      core::NativeWindow, std::uint64_t,
                                                                                      std::uint64_t) override
            {
                ++replies;
                return replyResult;
            }
            [[nodiscard]] std::optional<core::PeerEnvelope> receive(std::uintptr_t, std::intptr_t) override
            {
                if (throwAllocation) {
                    throw std::bad_alloc{};
                }
                return std::exchange(payload, std::nullopt);
            }
            [[nodiscard]] std::expected<core::PeerTransportResult, core::Error> send(const core::Peer &,
                                                                                     const core::Drop &) override
            {
                ++sends;
                return core::PeerTransportResult{.sent = 1, .receiver = 1};
            }
            [[nodiscard]] core::PeerMenuChoice menu(core::NativeWindow, core::Point) override
            {
                return choice;
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

        HANDLE WINAPI failCreateEvent(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCWSTR)
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

        enum class JoinWaitResult : std::uint8_t
        {
            Success,
            WrongIndex,
            Failure,
        } joinWaitResult{};

        DWORD joinWaitFlags{};
        DWORD joinWaitTimeout{};
        int joinWaitCalls{};

        struct DelayedThreadState
        {
            HANDLE gate{};
            LPTHREAD_START_ROUTINE entry{};
            LPVOID parameter{};
            DWORD id{};
            int sleeps{};
            int entries{};
            int joins{};
            int windows{};
        } delayedThread;

        DWORD WINAPI enterDelayedThread(LPVOID)
        {
            static_cast<void>(WaitForSingleObject(delayedThread.gate, INFINITE));
            ++delayedThread.entries;
            return delayedThread.entry(delayedThread.parameter);
        }

        HANDLE WINAPI createDelayedThread(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE entry, LPVOID parameter,
                                          DWORD, LPDWORD id)
        {
            delayedThread.entry = entry;
            delayedThread.parameter = parameter;
            const HANDLE thread = CreateThread(nullptr, 0, enterDelayedThread, nullptr, 0, id);
            delayedThread.id = *id;
            return thread;
        }

        void WINAPI countSleep(DWORD)
        {
            ++delayedThread.sleeps;
        }

        HWND WINAPI countFailedWindow(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE,
                                      LPVOID)
        {
            ++delayedThread.windows;
            return nullptr;
        }

        HRESULT WINAPI releaseDelayedThread(DWORD, DWORD, ULONG count, LPHANDLE handles, LPDWORD index)
        {
            ++delayedThread.joins;
            if (count != 1 || SetEvent(delayedThread.gate) == FALSE) {
                return E_FAIL;
            }
            for (int attempt = 0; attempt < 200; ++attempt) {
                if (PostThreadMessageW(delayedThread.id, WM_QUIT, 0, 0) != FALSE ||
                    WaitForSingleObject(handles[0], 0) == WAIT_OBJECT_0) {
                    break;
                }
                Sleep(1);
            }
            if (WaitForSingleObject(handles[0], 5000) != WAIT_OBJECT_0) {
                return E_FAIL;
            }
            *index = 0;
            return S_OK;
        }

        HRESULT WINAPI joinThread(DWORD flags, DWORD timeout, ULONG count, LPHANDLE handles, LPDWORD index)
        {
            ++joinWaitCalls;
            joinWaitFlags = flags;
            joinWaitTimeout = timeout;
            if (joinWaitResult == JoinWaitResult::Failure || count != 1) {
                return E_FAIL;
            }
            if (WaitForSingleObject(handles[0], 5000) != WAIT_OBJECT_0) {
                return E_FAIL;
            }
            *index = joinWaitResult == JoinWaitResult::WrongIndex ? 1U : 0U;
            return S_OK;
        }

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
            calls.coWait = joinThread;
            return calls;
        }

        void resetHeadlessWindow()
        {
            headlessWindow = {};
            joinWaitResult = JoinWaitResult::Success;
            joinWaitFlags = 0;
            joinWaitTimeout = 0;
            joinWaitCalls = 0;
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
            Extraction extraction;
            Peers peers;
            const auto calls = headlessCalls();
            ToolWindow tool{screen, input, shell, dropSession, extraction, peers, calls};

            CHECK(tool.start());
            CHECK(tool.start());
            REQUIRE(tool.nativeWindow() != 0);
            const std::vector<std::wstring> paths{L"C:\\one.txt"};
            auto context = dropContext();
            context.host = core::HostWindow{42, {0, 0, 1, 1}, false};
            CHECK(tool.prepare(paths, core::Button::Left, true, context));
            CHECK(shell.preferredEffect == core::Effect::Copy);
            CHECK(peers.announcements == 1);
            CHECK(peers.source == tool.nativeWindow());
            CHECK(peers.target == peers.broadcastTarget());
            CHECK(peers.announcedNonce == 100);
            CHECK(tool.hasData());
            CHECK(dropSession.context->press == core::Cell{5, 5});
            CHECK(dropSession.context->host->handle == 42);
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
            CHECK(extraction.cleanups == 1);
            CHECK(peers.endings == 1);

            tool.stop();
            tool.stop();
            CHECK(tool.nativeWindow() == 0);
            CHECK(joinWaitCalls == 1);
            CHECK(joinWaitTimeout == INFINITE);
            CHECK((joinWaitFlags & COWAIT_DISPATCH_CALLS) != 0);
            CHECK((joinWaitFlags & COWAIT_DISPATCH_WINDOW_MESSAGES) != 0);
        }

        TEST_CASE("the tool window routes foreign announcements, hellos, drops, and malformed copy data")
        {
            resetHeadlessWindow();
            Screen screen;
            Input input;
            Shell shell;
            DropSession dropSession;
            Extraction extraction;
            Peers peers;
            const auto calls = headlessCalls();
            ToolWindow tool{screen, input, shell, dropSession, extraction, peers, calls};
            REQUIRE(tool.start());
            const auto window = reinterpret_cast<HWND>(tool.nativeWindow());
            const std::vector<std::wstring> paths{L"C:\\one.txt"};
            REQUIRE(tool.prepare(paths, core::Button::Left, false, dropContext()));

            CHECK(SendMessageW(window, WM_APP + 77, 0, 0) == 1);
            peers.announcement = core::PeerAnnouncement{.action = core::PeerAnnouncementAction::Begin,
                                                        .source = {.window = 123, .process = peers.processId()},
                                                        .nonce = 300};
            CHECK(SendMessageW(window, WM_APP + 77, 0, 0) == 1);
            CHECK(peers.replies == 0);
            peers.announcement = core::PeerAnnouncement{.action = core::PeerAnnouncementAction::Begin,
                                                        .source = {.window = 123, .process = peers.processId() + 1},
                                                        .nonce = 300};
            CHECK(SendMessageW(window, WM_APP + 77, 0, 0) == 1);
            CHECK(peers.replies == 1);
            CHECK(SendMessageW(window, WM_COPYDATA, 0, 0) == 0);

            std::byte wire{};
            COPYDATASTRUCT copy{.dwData = 1, .cbData = 1, .lpData = &wire};
            CHECK(SendMessageW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copy)) == 0);
            peers.payload = core::PeerEnvelope{
                .sender = {.window = 3, .process = 2},
                .payload = core::PeerHello{
                    .process = 2, .tool = 3, .host = 4, .lastFocus = 5, .echoNonce = 100, .nonce = 400}};
            CHECK(SendMessageW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copy)) == 1);
            CHECK(dropSession.peerDrops.empty());
            const core::Drop drop{.paths = {L"C:\\one.txt"}, .at = {6, 7}, .effect = core::Effect::Move, .nonce = 100};
            peers.payload =
                core::PeerEnvelope{.sender = {.window = 123, .process = peers.processId() + 1}, .payload = drop};
            CHECK(SendMessageW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copy)) == 1);
            REQUIRE(dropSession.peerDrops.size() == 1);
            CHECK(dropSession.peerDrops[0] ==
                  core::PendingPeerDrop{.drop = drop, .sourceProcess = peers.processId() + 1});

            peers.announcement = core::PeerAnnouncement{.action = core::PeerAnnouncementAction::End,
                                                        .source = {.window = 123, .process = peers.processId() + 1},
                                                        .nonce = 300};
            CHECK(SendMessageW(window, WM_APP + 77, 0, 0) == 1);

            peers.payload = core::PeerEnvelope{
                .sender = {.window = 3, .process = 2},
                .payload = core::PeerHello{
                    .process = 2, .tool = 3, .host = 4, .lastFocus = 5, .echoNonce = 999, .nonce = 400}};
            CHECK(SendMessageW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copy)) == 0);

            peers.nonce = std::unexpected(core::Error::Unavailable);
            peers.announcement = core::PeerAnnouncement{
                .action = core::PeerAnnouncementAction::Begin, .source = {.window = 124, .process = 12}, .nonce = 301};
            CHECK(SendMessageW(window, WM_APP + 77, 0, 0) == 0);
            peers.nonce = 100;

            peers.announcement = core::PeerAnnouncement{
                .action = core::PeerAnnouncementAction::Begin, .source = {.window = 0, .process = 12}, .nonce = 302};
            CHECK(SendMessageW(window, WM_APP + 77, 0, 0) == 0);

            peers.replyResult = std::unexpected(core::Error::Unavailable);
            peers.announcement = core::PeerAnnouncement{
                .action = core::PeerAnnouncementAction::Begin, .source = {.window = 125, .process = 13}, .nonce = 303};
            CHECK(SendMessageW(window, WM_APP + 77, 0, 0) == 0);
            peers.replyResult =
                core::PeerTransportResult{.sent = 0, .receiver = 0, .lastError = 1460, .timeoutError = 1460};

            peers.announcement = core::PeerAnnouncement{
                .action = core::PeerAnnouncementAction::Begin, .source = {.window = 126, .process = 14}, .nonce = 304};
            CHECK(SendMessageW(window, WM_APP + 77, 0, 0) == 1);
            const core::Drop lateHello{
                .paths = {L"C:\\two.txt"}, .at = {6, 7}, .effect = core::Effect::Copy, .nonce = 100};
            peers.payload = core::PeerEnvelope{.sender = {.window = 126, .process = 14}, .payload = lateHello};
            CHECK(SendMessageW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copy)) == 1);
            CHECK(dropSession.peerDrops.size() == 2);

            peers.replyResult = core::PeerTransportResult{.sent = 1, .receiver = 1};
            peers.announcement = core::PeerAnnouncement{
                .action = core::PeerAnnouncementAction::Begin, .source = {.window = 127, .process = 15}, .nonce = 305};
            CHECK(SendMessageW(window, WM_APP + 77, 0, 0) == 1);
            dropSession.accepts = false;
            const core::Drop rejected{
                .paths = {L"C:\\three.txt"}, .at = {6, 7}, .effect = core::Effect::Copy, .nonce = 100};
            peers.payload = core::PeerEnvelope{.sender = {.window = 127, .process = 15}, .payload = rejected};
            CHECK(SendMessageW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copy)) == 0);
            CHECK(dropSession.peerDrops.size() == 3);
            dropSession.accepts = true;

            peers.nonce = std::unexpected(core::Error::Unavailable);
            CHECK(tool.prepare(paths, core::Button::Left, false, dropContext()));

            peers.throwAllocation = true;
            CHECK(SendMessageW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copy)) == 0);

            tool.stop();
        }

        TEST_CASE("private tool messages ignore foreign pointer-shaped parameters")
        {
            resetHeadlessWindow();
            Screen screen;
            screen.host = core::HostWindow{1, {100, 120, 420, 360}, false};
            Input input;
            Shell shell;
            DropSession dropSession;
            Extraction extraction;
            Peers peers;
            auto calls = headlessCalls();
            calls.sendMessage = sendPrivateWithHostileParameters;
            ToolWindow tool{screen, input, shell, dropSession, extraction, peers, calls};
            REQUIRE(tool.start());
            const auto window = reinterpret_cast<HWND>(tool.nativeWindow());
            constexpr LPARAM garbage = 1;

            CHECK(SendMessageW(window, WM_USER + 0x101, 0, garbage) == 0);
            CHECK(SendMessageW(window, WM_USER + 0x102, 0, garbage) == 0);
            CHECK(SendMessageW(window, WM_USER + 0x103, 0, garbage) == 0);
            CHECK(SendMessageW(window, WM_USER + 0x104, 0, garbage) == 0);
            CHECK(SendMessageW(window, WM_USER + 0x101, 0, 0) == 0);
            CHECK(SendMessageW(window, WM_USER + 0x102, 0, 0) == 0);
            CHECK(SendMessageW(window, WM_USER + 0x103, 0, 0) == 0);
            CHECK(SendMessageW(window, WM_USER + 0x104, 0, 0) == 0);
            privateMessageProbe = {};
            privateMessageProbe.input = &input;
            privateMessageProbe.extraction = &extraction;
            CHECK_FALSE(tool.hasData());

            const std::vector<std::wstring> paths{L"C:\\one.txt"};
            CHECK(tool.prepare(paths, core::Button::Left, true, dropContext()));
            CHECK(tool.hasData());
            CHECK(tool.showAndArm());
            CHECK(privateMessageProbe.startPressesBeforeLegitimate == 0);
            CHECK(input.presses.size() == 1);

            tool.abort();
            CHECK(privateMessageProbe.abortCleanupsBeforeLegitimate == 0);
            CHECK(extraction.cleanups == 1);
            CHECK_FALSE(tool.hasData());
            CHECK(privateMessageProbe.hostileResults.size() == 12);
            for (const auto result : privateMessageProbe.hostileResults) {
                CHECK(result == 0);
            }
            CHECK(IsWindow(window) != FALSE);
            tool.stop();
        }

        TEST_CASE("a successful plugin-panel peer handoff retains extracted files for the receiving synchro")
        {
            resetHeadlessWindow();
            Screen screen;
            screen.host = core::HostWindow{1, {100, 120, 420, 360}, false};
            screen.point = core::Point{10, 20};
            screen.root = 90;
            Input input;
            Shell shell;
            DropSession dropSession;
            Extraction extraction;
            Peers peers;
            peers.choice = core::PeerMenuChoice::Copy;
            const auto calls = headlessCalls();
            ToolWindow tool{screen, input, shell, dropSession, extraction, peers, calls};
            REQUIRE(tool.start());
            const std::vector<std::wstring> paths{L"C:\\one.txt"};
            REQUIRE(tool.prepare(paths, core::Button::Right, true, dropContext()));

            std::byte wire{};
            COPYDATASTRUCT copy{.dwData = 1, .cbData = 1, .lpData = &wire};
            peers.payload = core::PeerEnvelope{
                .sender = {.window = 91, .process = 2},
                .payload = core::PeerHello{
                    .process = 2, .tool = 91, .host = 90, .lastFocus = 5, .echoNonce = 100, .nonce = 200}};
            const auto window = reinterpret_cast<HWND>(tool.nativeWindow());
            REQUIRE(SendMessageW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copy)) == 1);
            REQUIRE(tool.showAndArm());
            shell.duringDrag = [&peers, &shell](core::NativeWindow) {
                auto &source = *reinterpret_cast<IDropSource *>(shell.sourceHandle);
                CHECK(source.GiveFeedback(DROPEFFECT_COPY) == DRAGDROP_S_USEDEFAULTCURSORS);
                CHECK(source.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_CANCEL);
                CHECK(peers.sends == 0);
            };

            SendMessageW(window, WM_RBUTTONDOWN, 0, 0);
            CHECK(peers.sends == 1);
            CHECK(extraction.cleanups == 0);
            CHECK(extraction.retains == 1);
            tool.stop();
        }

        TEST_CASE("ordinary plugin-panel drag outcomes route extracted runs to retention or cleanup")
        {
            resetHeadlessWindow();
            Screen screen;
            screen.host = core::HostWindow{1, {100, 120, 420, 360}, false};
            screen.point = core::Point{10, 20};
            screen.root = 90;
            Input input;
            Shell shell;
            DropSession dropSession;
            Extraction extraction;
            Peers peers;
            const auto calls = headlessCalls();
            ToolWindow tool{screen, input, shell, dropSession, extraction, peers, calls};
            REQUIRE(tool.start());
            const auto window = reinterpret_cast<HWND>(tool.nativeWindow());
            const std::vector<std::wstring> paths{L"C:\\one.txt"};
            HRESULT expectedQuery{DRAGDROP_S_DROP};
            bool escape{};
            int expectedExtractions{1};
            int expectedRetains{1};
            int expectedCleanups{};

            SUBCASE("completed external copy")
            {
                shell.dragOutcome = {DRAGDROP_S_DROP, DROPEFFECT_COPY};
            }
            SUBCASE("completed external move")
            {
                shell.dragOutcome = {DRAGDROP_S_DROP, DROPEFFECT_MOVE};
            }
            SUBCASE("drop reported no effect")
            {
                shell.dragOutcome = {DRAGDROP_S_DROP, DROPEFFECT_NONE};
                expectedRetains = 0;
                expectedCleanups = 1;
            }
            SUBCASE("Escape cancelled before extraction")
            {
                escape = true;
                expectedQuery = DRAGDROP_S_CANCEL;
                shell.dragOutcome = {DRAGDROP_S_CANCEL, DROPEFFECT_NONE};
                expectedExtractions = 0;
                expectedRetains = 0;
                expectedCleanups = 1;
            }
            SUBCASE("extraction failed")
            {
                extraction.succeeds = false;
                expectedQuery = DRAGDROP_S_CANCEL;
                shell.dragOutcome = {DRAGDROP_S_CANCEL, DROPEFFECT_NONE};
                expectedRetains = 0;
                expectedCleanups = 1;
            }
            SUBCASE("own tool-window drop never extracted")
            {
                screen.root = tool.nativeWindow();
                shell.dragOutcome = {DRAGDROP_S_DROP, DROPEFFECT_COPY};
                expectedExtractions = 0;
                expectedRetains = 0;
                expectedCleanups = 1;
            }

            REQUIRE(tool.prepare(paths, core::Button::Left, true, dropContext()));
            REQUIRE(tool.showAndArm());
            shell.duringDrag = [&](core::NativeWindow) {
                auto &source = *reinterpret_cast<IDropSource *>(shell.sourceHandle);
                CHECK(source.GiveFeedback(DROPEFFECT_COPY) == DRAGDROP_S_USEDEFAULTCURSORS);
                CHECK(source.QueryContinueDrag(escape, 0) == expectedQuery);
            };

            SendMessageW(window, WM_LBUTTONDOWN, 0, 0);
            CHECK(extraction.calls == expectedExtractions);
            CHECK(extraction.retains == expectedRetains);
            CHECK(extraction.cleanups == expectedCleanups);
            tool.stop();
        }

        TEST_CASE("a completed drag stays active until its extracted run is retained and touched")
        {
            resetHeadlessWindow();
            Screen screen;
            screen.host = core::HostWindow{1, {100, 120, 420, 360}, false};
            screen.point = core::Point{10, 20};
            screen.root = 90;
            Input input;
            Shell shell;
            shell.dragOutcome = {DRAGDROP_S_DROP, DROPEFFECT_COPY};
            DropSession dropSession;
            Extraction extraction;
            Peers peers;
            const auto calls = headlessCalls();
            ToolWindow tool{screen, input, shell, dropSession, extraction, peers, calls};
            REQUIRE(tool.start());
            const auto window = reinterpret_cast<HWND>(tool.nativeWindow());
            const std::vector<std::wstring> paths{L"C:\\one.txt"};
            REQUIRE(tool.prepare(paths, core::Button::Left, true, dropContext()));
            REQUIRE(tool.showAndArm());
            shell.duringDrag = [&shell](core::NativeWindow) {
                auto &source = *reinterpret_cast<IDropSource *>(shell.sourceHandle);
                CHECK(source.GiveFeedback(DROPEFFECT_COPY) == DRAGDROP_S_USEDEFAULTCURSORS);
                CHECK(source.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_DROP);
            };
            bool newGestureAccepted{};
            extraction.duringRetain = [&] { newGestureAccepted = !tool.active(); };

            SendMessageW(window, WM_LBUTTONDOWN, 0, 0);

            CHECK_FALSE(newGestureAccepted);
            CHECK(extraction.retains == 1);
            CHECK(extraction.touched);
            CHECK_FALSE(tool.active());
            tool.stop();
        }

        TEST_CASE("shutdown still joins the tool thread when its pumping wait reports an unusable result")
        {
            resetHeadlessWindow();
            SUBCASE("wait failed")
            {
                joinWaitResult = JoinWaitResult::Failure;
            }
            SUBCASE("wait returned an impossible handle index")
            {
                joinWaitResult = JoinWaitResult::WrongIndex;
            }

            Screen screen;
            Input input;
            Shell shell;
            DropSession dropSession;
            Extraction extraction;
            Peers peers;
            const auto calls = headlessCalls();
            ToolWindow tool{screen, input, shell, dropSession, extraction, peers, calls};
            REQUIRE(tool.start());

            tool.stop();

            CHECK(tool.nativeWindow() == 0);
            CHECK(joinWaitCalls == 1);
            CHECK(joinWaitTimeout == INFINITE);
        }

        TEST_CASE("a startup readiness timeout stops and joins a worker before returning")
        {
            delayedThread = {};
            delayedThread.gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            REQUIRE(delayedThread.gate != nullptr);
            {
                Screen screen;
                Input input;
                Shell shell;
                DropSession dropSession;
                Extraction extraction;
                Peers peers;
                auto calls = systemToolWindowCalls();
                calls.createThread = createDelayedThread;
                calls.sleep = countSleep;
                calls.createWindow = countFailedWindow;
                calls.coWait = releaseDelayedThread;
                ToolWindow tool{screen, input, shell, dropSession, extraction, peers, calls};

                CHECK_FALSE(tool.start());
                CHECK(delayedThread.sleeps == 200);
                CHECK(delayedThread.joins == 1);
                CHECK(delayedThread.entries == 1);
                CHECK(delayedThread.windows == 0);
                CHECK(tool.nativeWindow() == 0);
            }
            CloseHandle(delayedThread.gate);
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
            Extraction extraction;
            Peers peers;
            ToolWindow tool{screen, input, shell, dropSession, extraction, peers};
            REQUIRE(tool.start());
            const std::vector<std::wstring> paths{L"C:\\one.txt"};
            REQUIRE(tool.prepare(paths, core::Button::Left, false, dropContext()));
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
            Extraction extraction;
            Peers peers;
            ToolWindow tool{screen, input, shell, dropSession, extraction, peers};
            REQUIRE(tool.start());
            CHECK_FALSE(tool.showAndArm());
            SendMessageW(reinterpret_cast<HWND>(tool.nativeWindow()), WM_LBUTTONDOWN, 0, 0);
            CHECK_FALSE(tool.active());

            shell.prepares = false;
            const std::vector<std::wstring> invalid{L"Z:\\definitely-missing\\file.txt"};
            CHECK_FALSE(tool.prepare(invalid, core::Button::Right, false, dropContext()));
            shell.prepares = true;
            shell.parsedPaths = 1;
            const std::vector<std::wstring> valid{L"C:\\one.txt"};
            REQUIRE(tool.prepare(valid, core::Button::Right, false, dropContext()));
            CHECK_FALSE(tool.showAndArm());
            CHECK_FALSE(tool.hasData());
            CHECK(input.presses.empty());

            REQUIRE(tool.prepare(valid, core::Button::Right, false, dropContext()));
            tool.abort();
            CHECK_FALSE(tool.hasData());
            tool.stop();
        }

        TEST_CASE("prepare rejects a shell payload that omitted any requested path")
        {
            Screen screen;
            Input input;
            Shell shell;
            shell.parsedPaths = 1;
            DropSession dropSession;
            Extraction extraction;
            Peers peers;
            ToolWindow tool{screen, input, shell, dropSession, extraction, peers};
            REQUIRE(tool.start());
            const std::vector<std::wstring> paths{L"C:\\one.txt", L"C:\\two.txt"};

            CHECK_FALSE(tool.prepare(paths, core::Button::Left, false, dropContext()));
            CHECK_FALSE(tool.hasData());
            CHECK_FALSE(dropSession.context.has_value());
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
            Extraction extraction;
            Peers peers;
            const auto calls = headlessCalls();
            ToolWindow tool{screen, input, shell, dropSession, extraction, peers, calls};
            REQUIRE(tool.start());
            const std::vector<std::wstring> paths{L"C:\\one.txt"};
            REQUIRE(tool.prepare(paths, core::Button::Right, true, dropContext()));
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
            CHECK_FALSE(shell.allowedLink);
            CHECK_FALSE(tool.active());
            CHECK_FALSE(tool.hasData());
            CHECK_FALSE(headlessWindow.visible);
            CHECK(extraction.cleanups == 1);

            screen.host->topmost = false;
            REQUIRE(tool.prepare(paths, core::Button::Left, false, dropContext()));
            REQUIRE(tool.showAndArm());
            CHECK(headlessWindow.placements == 3);
            shell.duringDrag = [&tool](core::NativeWindow owner) {
                CHECK(tool.active());
                SendMessageW(reinterpret_cast<HWND>(owner), WM_LBUTTONDOWN, 0, 0);
            };
            SendMessageW(window, WM_LBUTTONDOWN, 0, 0);
            CHECK(shell.dragCalls == 2);
            CHECK(shell.allowedLink);

            tool.stop();
        }

        TEST_CASE("thread and native-window startup failures remain expected")
        {
            Screen screen;
            Input input;
            Shell shell;
            DropSession dropSession;
            Extraction extraction;
            Peers peers;

            auto calls = systemToolWindowCalls();
            calls.createEvent = failCreateEvent;
            ToolWindow noReadinessEvent{screen, input, shell, dropSession, extraction, peers, calls};
            CHECK_FALSE(noReadinessEvent.start());

            calls = systemToolWindowCalls();
            calls.createThread = failCreateThread;
            ToolWindow noThread{screen, input, shell, dropSession, extraction, peers, calls};
            CHECK_FALSE(noThread.start());

            calls = systemToolWindowCalls();
            calls.createWindow = failCreateWindow;
            calls.sleep = shortSleep;
            ToolWindow noWindow{screen, input, shell, dropSession, extraction, peers, calls};
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
            Extraction extraction;
            Peers peers;
            auto calls = headlessCalls();
            calls.isWindowVisible = reportInvisible;
            ToolWindow tool{screen, input, shell, dropSession, extraction, peers, calls};
            REQUIRE(tool.start());
            const std::vector<std::wstring> paths{L"C:\\one.txt"};
            REQUIRE(tool.prepare(paths, core::Button::Left, true, dropContext()));
            CHECK_FALSE(tool.showAndArm());
            CHECK_FALSE(tool.hasData());
            CHECK(extraction.cleanups == 1);

            const HWND withoutState = CreateWindowExW(0, L"BurlakToolWindow", L"", WS_POPUP, 0, 0, 1, 1, nullptr,
                                                      nullptr, GetModuleHandleW(nullptr), nullptr);
            REQUIRE(withoutState != nullptr);
            DestroyWindow(withoutState);

            tool.stop();
        }
    }

} // namespace burlak::drag
