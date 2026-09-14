#include "drag/DragSource.hpp"
#include "drag/ExtractionWait.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <thread>

namespace burlak::drag
{

    namespace
    {

        class Policy final : public core::IReleasePolicy
        {
          public:
            core::DragAction action{core::DragAction::Continue};
            core::Effect effect{core::Effect::None};
            mutable core::Button button{};
            mutable bool escape{};
            mutable bool left{};
            mutable bool right{};
            mutable core::Effect lastEffect{};
            mutable bool needsExtraction{};
            mutable bool overOwnWindow{};
            mutable bool overPeer{};

            [[nodiscard]] core::DragAction query(core::Button value, bool escapePressed, bool leftDown, bool rightDown,
                                                 core::Effect feedback, bool extraction, bool ownWindow,
                                                 bool peer) const override
            {
                button = value;
                escape = escapePressed;
                left = leftDown;
                right = rightDown;
                lastEffect = feedback;
                needsExtraction = extraction;
                overOwnWindow = ownWindow;
                overPeer = peer;
                return action;
            }

            [[nodiscard]] core::Effect feedback(bool move, bool copy, bool link) const override
            {
                left = move;
                right = copy;
                escape = link;
                return effect;
            }
        };

        class Screen final : public core::IScreen
        {
          public:
            std::optional<core::Point> point{core::Point{10, 20}};
            core::NativeWindow window{9};
            int cursorCalls{};

            [[nodiscard]] std::optional<core::Point> cursor() override
            {
                ++cursorCalls;
                return point;
            }

            [[nodiscard]] bool buttonDown(core::Button) override
            {
                return false;
            }

            [[nodiscard]] core::NativeWindow windowAt(core::Point) override
            {
                return window;
            }

            [[nodiscard]] std::optional<core::HostWindow> hostWindow() override
            {
                return std::nullopt;
            }

            [[nodiscard]] std::optional<core::HostWindow> hostWindowAt(core::Point) override
            {
                return std::nullopt;
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

        class Extraction final : public core::IExtraction
        {
          public:
            bool succeeds{true};
            int calls{};
            int cleanups{};
            std::vector<std::string> *callLog{};

            [[nodiscard]] bool extract() override
            {
                ++calls;
                if (callLog != nullptr) {
                    callLog->emplace_back("extract");
                }
                return succeeds;
            }

            void cleanup() override
            {
                ++cleanups;
            }

            void retain() override
            {
            }
        };

        class Peers final : public core::IPeers
        {
          public:
            core::PeerMenuChoice choice{core::PeerMenuChoice::Copy};
            std::vector<core::Drop> drops;
            std::vector<std::string> *callLog{};
            std::expected<core::PeerTransportResult, core::Error> sendResult{
                core::PeerTransportResult{.sent = 1, .receiver = 1}};

            [[nodiscard]] std::wstring_view toolWindowClass() const override
            {
                return L"BurlakToolWindow";
            }
            [[nodiscard]] bool isAnnouncementMessage(std::uint32_t) const override
            {
                return false;
            }
            [[nodiscard]] std::optional<core::PeerAnnouncement> receiveAnnouncement(std::uint32_t, std::uintptr_t,
                                                                                    std::intptr_t) override
            {
                return std::nullopt;
            }
            [[nodiscard]] std::expected<std::uint64_t, core::Error> newNonce() const override
            {
                return 1;
            }
            [[nodiscard]] std::uint32_t processId() const override
            {
                return 2;
            }
            [[nodiscard]] core::NativeWindow broadcastTarget() const override
            {
                return 3;
            }
            void announce(core::NativeWindow, core::NativeWindow, std::uint64_t) override
            {
            }
            void endAnnouncement(core::NativeWindow, core::NativeWindow, std::uint64_t) override
            {
            }
            [[nodiscard]] std::expected<core::PeerTransportResult, core::Error> reply(core::PeerIdentity,
                                                                                      core::NativeWindow, std::uint64_t,
                                                                                      std::uint64_t) override
            {
                return core::PeerTransportResult{.sent = 1, .receiver = 1};
            }
            [[nodiscard]] std::optional<core::PeerEnvelope> receive(std::uintptr_t, std::intptr_t) override
            {
                return std::nullopt;
            }
            [[nodiscard]] std::expected<core::PeerTransportResult, core::Error> send(const core::Peer &,
                                                                                     const core::Drop &drop) override
            {
                if (callLog != nullptr) {
                    callLog->emplace_back("send");
                }
                drops.push_back(drop);
                return sendResult;
            }
            [[nodiscard]] core::PeerMenuChoice menu(core::NativeWindow, core::Point) override
            {
                if (callLog != nullptr) {
                    callLog->emplace_back("menu");
                }
                return choice;
            }
        };

        class ExtractionHost final : public core::IExtractionSession
        {
          public:
            int requests{};

            [[nodiscard]] bool requestExtraction() override
            {
                ++requests;
                return true;
            }

            void cleanup() override
            {
            }

            void retain() override
            {
            }
        };

        class PostedExtractionHost final : public core::IExtractionSession
        {
          public:
            std::atomic<int> requests{};

            [[nodiscard]] bool requestExtraction() override
            {
                ++requests;
                return true;
            }

            void cleanup() override
            {
            }

            void retain() override
            {
            }
        };

        ExtractionWait *activeWait{};
        bool waitOutcome{};
        bool scopedOutcome{};

        HANDLE WINAPI createEvent(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCWSTR)
        {
            return CreateEventW(nullptr, TRUE, FALSE, nullptr);
        }

        HRESULT WINAPI waitForExtraction(DWORD, DWORD, ULONG, LPHANDLE, LPDWORD index)
        {
            *index = 0;
            activeWait->complete(waitOutcome);
            return S_OK;
        }

        HRESULT WINAPI waitForExceptionalCompletion(DWORD, DWORD, ULONG, LPHANDLE handles, LPDWORD index)
        {
            bool caught{};
            try {
                ExtractionCompleter completion{*activeWait};
                throw 1;
            } catch (...) {
                caught = true;
            }
            if (!caught) {
                return E_FAIL;
            }
            *index = 0;
            return WaitForSingleObject(handles[0], 0) == WAIT_OBJECT_0 ? S_OK : E_FAIL;
        }

        HRESULT WINAPI waitForScopedCompletion(DWORD, DWORD, ULONG, LPHANDLE handles, LPDWORD index)
        {
            {
                ExtractionCompleter completion{*activeWait};
                completion.complete(std::optional{scopedOutcome});
            }
            *index = 0;
            return WaitForSingleObject(handles[0], 0) == WAIT_OBJECT_0 ? S_OK : E_FAIL;
        }

        HRESULT WINAPI waitForShutdown(DWORD, DWORD, ULONG, LPHANDLE handles, LPDWORD index)
        {
            if (WaitForSingleObject(handles[0], INFINITE) != WAIT_OBJECT_0) {
                return E_FAIL;
            }
            *index = 0;
            return S_OK;
        }

    } // namespace

    TEST_SUITE("drag source")
    {
        TEST_CASE("QueryInterface follows COM identity and reference counting")
        {
            Policy policy;
            Screen screen;
            Extraction extraction;
            Peers peers;
            core::PeerRegistry registry;
            DragSource source{policy, screen, extraction, peers, registry, core::Button::Left, 7, 8, false, {}};
            CHECK(source.QueryInterface(IID_IUnknown, nullptr) == E_POINTER);

            void *object{};
            CHECK(source.QueryInterface(IID_IUnknown, &object) == S_OK);
            CHECK(object == static_cast<IDropSource *>(&source));
            CHECK(source.Release() == 1);

            object = nullptr;
            CHECK(source.QueryInterface(IID_IDropSource, &object) == S_OK);
            CHECK(object == static_cast<IDropSource *>(&source));
            CHECK(source.Release() == 1);

            const GUID unknown{0x12345678, 0, 0, {}};
            CHECK(source.QueryInterface(unknown, &object) == E_NOINTERFACE);
            CHECK(object == nullptr);
            CHECK(source.AddRef() == 2);
            CHECK(source.Release() == 1);
        }

        TEST_CASE("QueryContinueDrag marshals the button state to the core policy")
        {
            Policy policy;
            Screen screen;
            Extraction extraction;
            Peers peers;
            core::PeerRegistry registry;
            DragSource source{policy, screen, extraction, peers, registry, core::Button::Right, 7, 8, true, {}};
            policy.action = core::DragAction::Cancel;
            CHECK(source.QueryContinueDrag(TRUE, MK_LBUTTON | MK_RBUTTON) == DRAGDROP_S_CANCEL);
            CHECK(policy.button == core::Button::Right);
            CHECK(policy.escape);
            CHECK(policy.left);
            CHECK(policy.right);
            CHECK(policy.needsExtraction);
            CHECK_FALSE(policy.overOwnWindow);
            policy.action = core::DragAction::Drop;
            CHECK(source.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_DROP);
            policy.action = core::DragAction::Continue;
            CHECK(source.QueryContinueDrag(FALSE, MK_RBUTTON) == S_OK);
        }

        TEST_CASE("feedback records the latest effect and keeps the system cursors")
        {
            Policy policy;
            Screen screen;
            Extraction extraction;
            Peers peers;
            core::PeerRegistry registry;
            DragSource source{policy, screen, extraction, peers, registry, core::Button::Left, 7, 8, false, {}};
            policy.effect = core::Effect::Copy;
            CHECK(source.GiveFeedback(DROPEFFECT_COPY) == DRAGDROP_S_USEDEFAULTCURSORS);
            CHECK(source.lastEffect() == core::Effect::Copy);
            CHECK_FALSE(policy.left);
            CHECK(policy.right);
            CHECK_FALSE(policy.escape);
            policy.effect = core::Effect::Move;
            CHECK(source.GiveFeedback(DROPEFFECT_MOVE) == DRAGDROP_S_USEDEFAULTCURSORS);
            CHECK(source.lastEffect() == core::Effect::Move);
            policy.effect = core::Effect::None;
            CHECK(source.GiveFeedback(DROPEFFECT_NONE) == DRAGDROP_S_USEDEFAULTCURSORS);
            CHECK(source.lastEffect() == core::Effect::None);
            policy.effect = core::Effect::Link;
            CHECK(source.GiveFeedback(DROPEFFECT_LINK) == DRAGDROP_S_USEDEFAULTCURSORS);
            CHECK(source.lastEffect() == core::Effect::Link);
            CHECK(policy.escape);
        }

        TEST_CASE("placeholder-backed feedback never accepts a link effect")
        {
            Policy policy;
            Screen screen;
            Extraction extraction;
            Peers peers;
            core::PeerRegistry registry;
            DragSource source{policy, screen, extraction, peers, registry, core::Button::Left, 7, 8, true, {}};
            policy.effect = core::Effect::Link;

            CHECK(source.GiveFeedback(DROPEFFECT_LINK) == DRAGDROP_S_USEDEFAULTCURSORS);
            CHECK_FALSE(policy.escape);
        }

        TEST_CASE("an external plugin-panel release waits for extraction and maps its outcome")
        {
            Policy policy;
            policy.action = core::DragAction::ExtractThenDrop;
            policy.effect = core::Effect::Copy;
            Screen screen;
            ExtractionHost host;
            const ExtractionWaitCalls calls{createEvent, ResetEvent, SetEvent, CloseHandle, waitForExtraction};
            ExtractionWait extraction{host, calls};
            activeWait = &extraction;
            Peers peers;
            core::PeerRegistry registry;
            DragSource source{policy, screen, extraction, peers, registry, core::Button::Left, 7, 8, true, {}};
            REQUIRE(source.GiveFeedback(DROPEFFECT_COPY) == DRAGDROP_S_USEDEFAULTCURSORS);

            waitOutcome = true;
            CHECK(source.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_DROP);
            CHECK(host.requests == 1);

            waitOutcome = false;
            CHECK(source.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_CANCEL);
            CHECK(host.requests == 2);
        }

        TEST_CASE("an exceptional Far-thread exit completes the extraction wait with failure")
        {
            ExtractionHost host;
            const ExtractionWaitCalls calls{createEvent, ResetEvent, SetEvent, CloseHandle,
                                            waitForExceptionalCompletion};
            ExtractionWait extraction{host, calls};
            activeWait = &extraction;

            CHECK_FALSE(extraction.extract());
            CHECK(host.requests == 1);
        }

        TEST_CASE("shutdown after a posted extraction request wakes the tool-thread wait")
        {
            PostedExtractionHost host;
            const ExtractionWaitCalls calls{createEvent, ResetEvent, SetEvent, CloseHandle, waitForShutdown};
            ExtractionWait extraction{host, calls};
            bool outcome{true};
            std::jthread toolThread{[&] { outcome = extraction.extract(); }};

            for (int attempt = 0; attempt != 1000 && host.requests.load() == 0; ++attempt) {
                Sleep(1);
            }
            const bool posted = host.requests.load() == 1;
            extraction.cancel();
            toolThread.join();

            REQUIRE(posted);
            CHECK_FALSE(outcome);
            CHECK_FALSE(extraction.extract());
            CHECK(host.requests.load() == 1);
        }

        TEST_CASE("a completed Far-thread scope signals exactly the stored extraction outcome")
        {
            ExtractionHost host;
            const ExtractionWaitCalls calls{createEvent, ResetEvent, SetEvent, CloseHandle, waitForScopedCompletion};
            ExtractionWait extraction{host, calls};
            activeWait = &extraction;

            scopedOutcome = true;
            CHECK(extraction.extract());
            scopedOutcome = false;
            CHECK_FALSE(extraction.extract());
            CHECK(host.requests == 2);
        }

        TEST_CASE("the release policy sees whether the cursor is over Burlak's own window")
        {
            Policy policy;
            policy.action = core::DragAction::Drop;
            policy.effect = core::Effect::Move;
            Screen screen;
            screen.window = 7;
            Extraction extraction;
            Peers peers;
            core::PeerRegistry registry;
            DragSource source{policy, screen, extraction, peers, registry, core::Button::Left, 7, 8, true, {}};
            REQUIRE(source.GiveFeedback(DROPEFFECT_MOVE) == DRAGDROP_S_USEDEFAULTCURSORS);

            CHECK(source.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_DROP);
            CHECK(policy.overOwnWindow);
            CHECK(policy.lastEffect == core::Effect::Move);
            CHECK(extraction.calls == 0);

            screen.point.reset();
            CHECK(source.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_DROP);
            CHECK_FALSE(policy.overOwnWindow);
        }

        TEST_CASE("an accepted real-path release checks for a peer beneath the cursor")
        {
            Policy policy;
            policy.action = core::DragAction::Drop;
            policy.effect = core::Effect::Move;
            Screen screen;
            Extraction extraction;
            Peers peers;
            core::PeerRegistry registry;
            DragSource source{policy, screen, extraction, peers, registry, core::Button::Left, 7, 8, false, {}};
            REQUIRE(source.GiveFeedback(DROPEFFECT_MOVE) == DRAGDROP_S_USEDEFAULTCURSORS);

            CHECK(source.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_DROP);
            CHECK_FALSE(policy.needsExtraction);
            CHECK(screen.cursorCalls == 1);
        }

        TEST_CASE("peer release extracts, asks a right-drag choice, cancels OLE, and then sends")
        {
            core::ReleasePolicy policy;
            Screen screen;
            screen.window = 90;
            Extraction extraction;
            Peers peers;
            core::PeerRegistry registry;
            registry.begin(80);
            REQUIRE(registry.add(
                core::PeerHello{.process = 9, .tool = 91, .host = 90, .lastFocus = 7, .echoNonce = 80, .nonce = 81},
                core::PeerIdentity{.window = 91, .process = 9}));
            const std::vector<std::wstring> paths{L"C:\\one.txt", L"C:\\two.txt"};
            std::vector<std::string> calls;
            extraction.callLog = &calls;
            peers.callLog = &calls;
            DragSource source{policy, screen, extraction, peers, registry, core::Button::Right, 7, 8, true, paths};
            REQUIRE(source.GiveFeedback(DROPEFFECT_COPY) == DRAGDROP_S_USEDEFAULTCURSORS);

            CHECK(source.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_CANCEL);
            calls.emplace_back("cancel");
            CHECK(calls == std::vector<std::string>{"extract", "menu", "cancel"});
            CHECK_FALSE(source.peerHandoff());
            CHECK(peers.drops.empty());
            source.completePeerHandoff();
            CHECK(calls == std::vector<std::string>{"extract", "menu", "cancel", "send"});
            CHECK(source.peerHandoff());
            REQUIRE(peers.drops.size() == 1);
            CHECK(peers.drops[0] ==
                  core::Drop{.paths = paths, .at = {10, 20}, .effect = core::Effect::Copy, .nonce = 81});
        }

        TEST_CASE("peer release maps Shift to move and cancellation or extraction failure never sends")
        {
            core::ReleasePolicy policy;
            Screen screen;
            screen.window = 90;
            Peers peers;
            core::PeerRegistry registry;
            registry.begin(80);
            REQUIRE(registry.add(
                core::PeerHello{.process = 9, .tool = 91, .host = 90, .lastFocus = 0, .echoNonce = 80, .nonce = 81},
                core::PeerIdentity{.window = 91, .process = 9}));
            const std::vector<std::wstring> paths{L"C:\\one.txt"};

            SUBCASE("left Shift move")
            {
                Extraction extraction;
                DragSource source{policy, screen, extraction, peers, registry, core::Button::Left, 7, 8, false, paths};
                REQUIRE(source.GiveFeedback(DROPEFFECT_COPY) == DRAGDROP_S_USEDEFAULTCURSORS);
                CHECK(source.QueryContinueDrag(FALSE, MK_SHIFT) == DRAGDROP_S_CANCEL);
                source.completePeerHandoff();
                REQUIRE(peers.drops.size() == 1);
                CHECK(peers.drops[0].effect == core::Effect::Move);
            }
            SUBCASE("left copy")
            {
                Extraction extraction;
                DragSource source{policy, screen, extraction, peers, registry, core::Button::Left, 7, 8, false, paths};
                REQUIRE(source.GiveFeedback(DROPEFFECT_COPY) == DRAGDROP_S_USEDEFAULTCURSORS);
                CHECK(source.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_CANCEL);
                source.completePeerHandoff();
                REQUIRE(peers.drops.size() == 1);
                CHECK(peers.drops[0].effect == core::Effect::Copy);
            }
            SUBCASE("right menu cancellation")
            {
                Extraction extraction;
                peers.choice = core::PeerMenuChoice::Cancel;
                DragSource source{policy, screen, extraction, peers, registry, core::Button::Right, 7, 8, false, paths};
                REQUIRE(source.GiveFeedback(DROPEFFECT_COPY) == DRAGDROP_S_USEDEFAULTCURSORS);
                CHECK(source.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_CANCEL);
                source.completePeerHandoff();
                CHECK(peers.drops.empty());
                CHECK_FALSE(source.peerHandoff());
            }
            SUBCASE("extraction failure")
            {
                Extraction extraction;
                extraction.succeeds = false;
                DragSource source{policy, screen, extraction, peers, registry, core::Button::Right, 7, 8, true, paths};
                REQUIRE(source.GiveFeedback(DROPEFFECT_COPY) == DRAGDROP_S_USEDEFAULTCURSORS);
                CHECK(source.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_CANCEL);
                source.completePeerHandoff();
                CHECK(peers.drops.empty());
                CHECK_FALSE(source.peerHandoff());
            }
            SUBCASE("own host is never selected")
            {
                Extraction extraction;
                DragSource source{policy, screen, extraction, peers, registry, core::Button::Left, 7, 90, false, paths};
                REQUIRE(source.GiveFeedback(DROPEFFECT_COPY) == DRAGDROP_S_USEDEFAULTCURSORS);
                CHECK(source.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_DROP);
                CHECK(peers.drops.empty());
                CHECK_FALSE(source.peerHandoff());
            }
            SUBCASE("send failure")
            {
                Extraction extraction;
                peers.sendResult = std::unexpected(core::Error::Unavailable);
                DragSource source{policy, screen, extraction, peers, registry, core::Button::Left, 7, 8, false, paths};
                REQUIRE(source.GiveFeedback(DROPEFFECT_COPY) == DRAGDROP_S_USEDEFAULTCURSORS);
                CHECK(source.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_CANCEL);
                source.completePeerHandoff();
                CHECK_FALSE(source.peerHandoff());
            }
            SUBCASE("an indeterminate extracted send preserves the run")
            {
                Extraction extraction;
                peers.sendResult =
                    core::PeerTransportResult{.sent = 0, .receiver = 0, .lastError = 1460, .timeoutError = 1460};
                DragSource source{policy, screen, extraction, peers, registry, core::Button::Left, 7, 8, true, paths};
                REQUIRE(source.GiveFeedback(DROPEFFECT_COPY) == DRAGDROP_S_USEDEFAULTCURSORS);
                CHECK(source.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_CANCEL);
                source.completePeerHandoff();
                CHECK(source.peerHandoff());
            }
            SUBCASE("a definite extracted-send refusal does not preserve the run")
            {
                Extraction extraction;
                peers.sendResult =
                    core::PeerTransportResult{.sent = 0, .receiver = 0, .lastError = 5, .timeoutError = 1460};
                DragSource source{policy, screen, extraction, peers, registry, core::Button::Left, 7, 8, true, paths};
                REQUIRE(source.GiveFeedback(DROPEFFECT_COPY) == DRAGDROP_S_USEDEFAULTCURSORS);
                CHECK(source.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_CANCEL);
                source.completePeerHandoff();
                CHECK_FALSE(source.peerHandoff());
            }
        }
    }

} // namespace burlak::drag
