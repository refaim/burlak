#include "drag/ToolWindow.hpp"

#include "core/Peers.hpp"
#include "core/Policies.hpp"
#include "drag/DragSource.hpp"
#include "drag/DropTarget.hpp"

#include <windows.h>

#include <atomic>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

namespace burlak::drag
{

    namespace
    {

        constexpr UINT prepareDragMessage = WM_USER + 0x101;
        constexpr UINT startDragMessage = WM_USER + 0x102;
        constexpr UINT abortDragMessage = WM_USER + 0x103;
        constexpr UINT hasDataMessage = WM_USER + 0x104;
        constexpr UINT_PTR armTimer = 1;
        constexpr UINT armTimeoutMilliseconds = 1000;
        constexpr int startupPollAttempts = 200;
        constexpr DWORD startupPollMilliseconds = 5;
        const ToolWindowCalls systemCalls{CreateThread,    Sleep,        CreateEventW, CreateWindowExW,
                                          IsWindowVisible, SetWindowPos, ShowWindow,   SetCapture,
                                          ReleaseCapture,  SetTimer,     KillTimer,    CoWaitForMultipleHandles};

        struct HandleCloser
        {
            void operator()(void *handle) const noexcept
            {
                static_cast<void>(CloseHandle(handle));
            }
        };

        using UniqueHandle = std::unique_ptr<void, HandleCloser>;

        struct WindowDestroyer
        {
            void operator()(HWND window) const noexcept
            {
                static_cast<void>(DestroyWindow(window));
            }
        };

        struct DragDropRevoker
        {
            void operator()(HWND window) const noexcept
            {
                static_cast<void>(RevokeDragDrop(window));
            }
        };

        using WindowObject = std::remove_pointer_t<HWND>;
        using UniqueWindow = std::unique_ptr<WindowObject, WindowDestroyer>;
        using DragDropRegistration = std::unique_ptr<WindowObject, DragDropRevoker>;

        struct PreparePayload
        {
            std::span<const std::wstring> paths;
            core::Button button{core::Button::Left};
            bool needsExtraction{};
            core::DropContext context;
        };

    } // namespace

    class ToolWindow::State
    {
      public:
        State(core::IScreen &screen, core::IInput &input, core::IShell &shell, core::IDropSession &dropSession,
              core::IExtraction &extraction, core::IPeers &peers, const ToolWindowCalls &calls)
            : screen_{screen}, input_{input}, shell_{shell}, dropSession_{dropSession}, extraction_{extraction},
              peers_{peers}, calls_{calls}, dropTarget_{dropSession_}
        {
        }

        [[nodiscard]] bool start()
        {
            if (thread_.get() != nullptr) {
                return window_.load() != 0;
            }
            readiness_.reset(calls_.createEvent(nullptr, TRUE, FALSE, nullptr));
            if (readiness_.get() == nullptr) {
                return false;
            }
            stopRequested_.store(false);
            threadId_ = 0;
            thread_.reset(calls_.createThread(nullptr, 0, threadEntry, this, 0, &threadId_));
            if (thread_.get() == nullptr) {
                readiness_.reset();
                return false;
            }
            bool ready{};
            for (int attempt = 0; attempt < startupPollAttempts; ++attempt) {
                if (WaitForSingleObject(readiness_.get(), 0) == WAIT_OBJECT_0) {
                    ready = true;
                    break;
                }
                calls_.sleep(startupPollMilliseconds);
            }
            if (!ready) {
                stop();
                return false;
            }
            for (int attempt = 0; attempt < startupPollAttempts && window_.load() == 0; ++attempt) {
                calls_.sleep(startupPollMilliseconds);
            }
            if (window_.load() == 0) {
                stop();
                return false;
            }
            return true;
        }

        void stop()
        {
            stopRequested_.store(true);
            if (threadId_ != 0) {
                static_cast<void>(PostThreadMessageW(threadId_, WM_QUIT, 0, 0));
            }
            if (thread_.get() != nullptr) {
                HANDLE handle = thread_.get();
                DWORD signalled{};
                constexpr DWORD pumpFlags =
                    static_cast<DWORD>(COWAIT_DISPATCH_CALLS) | static_cast<DWORD>(COWAIT_DISPATCH_WINDOW_MESSAGES);
                // OLE can still own cross-apartment target calls after Drop returns. Pumping until the tool thread
                // has actually exited keeps State and its COM data alive throughout that unwind.
                const HRESULT status = calls_.coWait(pumpFlags, INFINITE, 1, &handle, &signalled);
                if (status != S_OK || signalled != 0) {
                    static_cast<void>(WaitForSingleObject(handle, INFINITE));
                }
                thread_.reset();
            }
            readiness_.reset();
            threadId_ = 0;
        }

        [[nodiscard]] bool prepare(std::span<const std::wstring> paths, core::Button button, bool needsExtraction,
                                   core::DropContext context)
        {
            // SendMessage is synchronous across these threads, so the path view remains alive while the
            // context value transfers the Far-thread snapshot before any hover or drop can read it.
            const PreparePayload payload{paths, button, needsExtraction, std::move(context)};
            return SendMessageW(windowHandle(), prepareDragMessage, 0, reinterpret_cast<LPARAM>(&payload)) != 0;
        }

        [[nodiscard]] bool showAndArm()
        {
            return SendMessageW(windowHandle(), startDragMessage, 0, 0) != 0;
        }

        void abort()
        {
            static_cast<void>(SendMessageW(windowHandle(), abortDragMessage, 0, 0));
        }

        [[nodiscard]] bool active() const
        {
            return active_.load();
        }

        [[nodiscard]] core::NativeWindow nativeWindow() const
        {
            return window_.load();
        }

        [[nodiscard]] bool hasData() const
        {
            return SendMessageW(windowHandle(), hasDataMessage, 0, 0) != 0;
        }

      private:
        [[nodiscard]] HWND windowHandle() const
        {
            return reinterpret_cast<HWND>(window_.load());
        }

        static DWORD WINAPI threadEntry(void *parameter)
        {
            return static_cast<State *>(parameter)->threadMain();
        }

        [[nodiscard]] DWORD threadMain()
        {
            MSG message{};
            // PeekMessage creates the queue before the main thread can rely on PostThreadMessage for shutdown.
            static_cast<void>(PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE));
            static_cast<void>(SetEvent(readiness_.get()));
            static_cast<void>(OleInitialize(nullptr));
            if (stopRequested_.load()) {
                OleUninitialize();
                return 0;
            }

            WNDCLASSEXW windowClass{};
            windowClass.cbSize = sizeof(windowClass);
            windowClass.lpfnWndProc = windowProcedure;
            windowClass.hInstance = GetModuleHandleW(nullptr);
            // IPeers exposes a view backed by a terminated class-name literal for these Win32 calls.
            windowClass.lpszClassName =
                peers_.toolWindowClass().data(); // NOLINT(bugprone-suspicious-stringview-data-usage)
            static_cast<void>(RegisterClassExW(&windowClass));

            UniqueWindow ownedWindow{calls_.createWindow(
                WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                peers_.toolWindowClass().data(), // NOLINT(bugprone-suspicious-stringview-data-usage)
                nullptr, WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, windowClass.hInstance, this)};
            const auto window = ownedWindow.get();
            DragDropRegistration registration;
            if (window != nullptr) {
                static_cast<void>(SetLayeredWindowAttributes(window, 0, 1, LWA_ALPHA));
                window_.store(reinterpret_cast<core::NativeWindow>(window));
                static_cast<void>(RegisterDragDrop(window, &dropTarget_));
                registration.reset(window);
            }

            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                static_cast<void>(TranslateMessage(&message));
                static_cast<void>(DispatchMessageW(&message));
            }

            clearDrag();
            registration.reset();
            ownedWindow.reset();
            window_.store(0);
            OleUninitialize();
            return 0;
        }

        static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM word, LPARAM number)
        {
            State *state = reinterpret_cast<State *>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (message == WM_NCCREATE) {
                const auto create = reinterpret_cast<CREATESTRUCTW *>(number);
                state = static_cast<State *>(create->lpCreateParams);
                SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            }
            if (state != nullptr) {
                try {
                    return state->handleMessage(window, message, word, number);
                } catch (const std::bad_alloc &) {
                    return 0;
                }
            }
            return DefWindowProcW(window, message, word, number);
        }

        LRESULT handleMessage(HWND window, UINT message, WPARAM word, LPARAM number)
        {
            if (peers_.isAnnouncementMessage(message)) {
                const auto announcement = peers_.receiveAnnouncement(message, word, number);
                if (!announcement || announcement->source.process == peers_.processId()) {
                    return 1;
                }
                if (announcement->action == core::PeerAnnouncementAction::End) {
                    incoming_.end(*announcement);
                    return 1;
                }
                const auto nonce = peers_.newNonce();
                if (!nonce || !incoming_.begin(*announcement, *nonce)) {
                    return 0;
                }
                if (!peers_.reply(announcement->source, reinterpret_cast<core::NativeWindow>(window),
                                  announcement->nonce, *nonce)) {
                    incoming_.end(core::PeerAnnouncement{.action = core::PeerAnnouncementAction::End,
                                                         .source = announcement->source,
                                                         .nonce = announcement->nonce});
                    return 0;
                }
                return 1;
            }
            if (message == WM_COPYDATA) {
                auto envelope = peers_.receive(word, number);
                if (!envelope) {
                    return 0;
                }
                if (std::holds_alternative<core::PeerHello>(envelope->payload)) {
                    return registry_.add(std::get<core::PeerHello>(envelope->payload), envelope->sender) ? 1 : 0;
                }
                auto accepted = incoming_.accept(envelope->sender, std::get<core::Drop>(std::move(envelope->payload)));
                return accepted && dropSession_.receivePeerDrop(std::move(*accepted)) ? 1 : 0;
            }
            switch (message) {
            case prepareDragMessage: {
                const auto &payload = *reinterpret_cast<const PreparePayload *>(number);
                clearDrag();
                auto prepared = shell_.makeDataObject(payload.paths);
                // Far's eventual copy consumes its live selection, so every selected path must also be present in
                // the OLE payload (Far source: far/filelist.cpp, FileList::ProcessCopyKeys).
                if (!prepared || !core::allPathsAdvertised(payload.paths.size(), prepared->parsedPaths)) {
                    return 0;
                }
                button_ = payload.button;
                needsExtraction_ = payload.needsExtraction;
                paths_.assign(payload.paths.begin(), payload.paths.end());
                ownHost_ = payload.context.host.transform([](const core::HostWindow &host) { return host.handle; })
                               .value_or(0);
                dropSession_.prepare(payload.context);
                data_ = std::move(prepared->data);
                if (const auto nonce = peers_.newNonce()) {
                    sourceNonce_ = *nonce;
                    registry_.begin(*nonce);
                    peers_.announce(reinterpret_cast<core::NativeWindow>(window), peers_.broadcastTarget(), *nonce);
                }
                return 1;
            }
            case startDragMessage:
                return showAndArm(window);
            case abortDragMessage:
                clearDrag();
                return 0;
            case hasDataMessage:
                return data_ ? 1 : 0;
            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN:
                calls_.killTimer(window, armTimer);
                runDrag(window);
                return 0;
            case WM_TIMER:
                if (word == armTimer) {
                    disarm(window);
                    return 0;
                }
                break;
            case WM_DESTROY:
                calls_.showWindow(window, SW_HIDE);
                return 0;
            default:
                break;
            }
            return DefWindowProcW(window, message, word, number);
        }

        [[nodiscard]] LRESULT showAndArm(HWND window)
        {
            const auto host = screen_.hostWindow();
            if (!data_ || !host) {
                clearDrag();
                return 0;
            }

            // Demotion is harmless when Windows refused an earlier topmost request, and avoids making
            // coverage depend on whether this process currently has foreground rights.
            const auto choice = core::placement(host->topmost);
            if (choice.demoteFirst) {
                calls_.setWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            calls_.setWindowPos(window, choice.topmost ? HWND_TOPMOST : HWND_TOP, host->rect.left, host->rect.top,
                                host->rect.right - host->rect.left, host->rect.bottom - host->rect.top,
                                SWP_NOACTIVATE | SWP_SHOWWINDOW);
            if (!calls_.isWindowVisible(window)) {
                clearDrag();
                return 0;
            }
            calls_.setCapture(window);
            calls_.setTimer(window, armTimer, armTimeoutMilliseconds, nullptr);
            // Session has just released the physical button; the queued synthetic press is therefore a
            // new click on the tool window, not a continuation of Far's panel gesture.
            input_.press(button_);
            return 1;
        }

        void runDrag(HWND window)
        {
            if (!data_ || active_.exchange(true)) {
                return;
            }
            DragSource source{releasePolicy_,
                              screen_,
                              extraction_,
                              peers_,
                              registry_,
                              button_,
                              reinterpret_cast<core::NativeWindow>(window),
                              ownHost_,
                              needsExtraction_,
                              paths_};
            // A target may report Move after taking the extracted placeholders; GetFilesW is always non-moving,
            // so the archive or remote panel remains untouched.
            static_cast<void>(shell_.runDrag(reinterpret_cast<core::NativeWindow>(window), *data_,
                                             reinterpret_cast<std::uintptr_t>(&source), !needsExtraction_));
            source.completePeerHandoff();
            active_.store(false);
            calls_.releaseCapture();
            calls_.showWindow(window, SW_HIDE);
            clearDrag(source.peerHandoff());
        }

        void disarm(HWND window)
        {
            calls_.killTimer(window, armTimer);
            if (active_.load()) {
                return;
            }
            calls_.releaseCapture();
            calls_.showWindow(window, SW_HIDE);
            clearDrag();
        }

        void clearDrag(bool preserveExtraction = false)
        {
            if (sourceNonce_) {
                peers_.endAnnouncement(window_.load(), peers_.broadcastTarget(), *sourceNonce_);
                sourceNonce_.reset();
            }
            registry_.end();
            data_.reset();
            paths_.clear();
            ownHost_ = 0;
            if (std::exchange(needsExtraction_, false)) {
                if (preserveExtraction) {
                    extraction_.retain();
                } else {
                    extraction_.cleanup();
                }
            }
        }

        core::IScreen &screen_;
        core::IInput &input_;
        core::IShell &shell_;
        core::IDropSession &dropSession_;
        core::IExtraction &extraction_;
        core::IPeers &peers_;
        const ToolWindowCalls &calls_;
        UniqueHandle thread_;
        UniqueHandle readiness_;
        DWORD threadId_{};
        std::atomic<core::NativeWindow> window_{};
        std::atomic<bool> active_{};
        std::atomic<bool> stopRequested_{};
        core::Button button_{core::Button::Left};
        bool needsExtraction_{};
        core::NativeWindow ownHost_{};
        std::vector<std::wstring> paths_;
        std::unique_ptr<core::IShell::DragData> data_;
        core::ReleasePolicy releasePolicy_;
        core::PeerRegistry registry_;
        core::IncomingPeerRegistry incoming_;
        std::optional<std::uint64_t> sourceNonce_;
        DropTarget dropTarget_;
    };

    ToolWindow::ToolWindow(core::IScreen &screen, core::IInput &input, core::IShell &shell,
                           core::IDropSession &dropSession, core::IExtraction &extraction, core::IPeers &peers)
        : state_{std::make_unique<State>(screen, input, shell, dropSession, extraction, peers, systemCalls)}
    {
    }

    ToolWindow::ToolWindow(core::IScreen &screen, core::IInput &input, core::IShell &shell,
                           core::IDropSession &dropSession, core::IExtraction &extraction, core::IPeers &peers,
                           const ToolWindowCalls &calls)
        : state_{std::make_unique<State>(screen, input, shell, dropSession, extraction, peers, calls)}
    {
    }

    ToolWindow::~ToolWindow()
    {
        stop();
    }

    bool ToolWindow::start()
    {
        return state_->start();
    }

    bool ToolWindow::prepare(std::span<const std::wstring> paths, core::Button button, bool needsExtraction,
                             core::DropContext context)
    {
        return state_->prepare(paths, button, needsExtraction, context);
    }

    bool ToolWindow::showAndArm()
    {
        return state_->showAndArm();
    }

    void ToolWindow::abort()
    {
        state_->abort();
    }

    bool ToolWindow::active() const
    {
        return state_->active();
    }

    void ToolWindow::stop()
    {
        state_->stop();
    }

    core::NativeWindow ToolWindow::nativeWindow() const
    {
        return state_->nativeWindow();
    }

    bool ToolWindow::hasData() const
    {
        return state_->hasData();
    }

    std::uintptr_t armTimerId()
    {
        return armTimer;
    }

    const ToolWindowCalls &systemToolWindowCalls()
    {
        return systemCalls;
    }

} // namespace burlak::drag
