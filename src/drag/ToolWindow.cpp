#include "drag/ToolWindow.hpp"

#include "core/Policies.hpp"
#include "drag/DragSource.hpp"
#include "drag/DropTarget.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace burlak::drag
{

    namespace
    {

        constexpr wchar_t toolClass[] = L"BurlakToolWindow";
        constexpr UINT prepareDragMessage = WM_USER + 0x101;
        constexpr UINT startDragMessage = WM_USER + 0x102;
        constexpr UINT abortDragMessage = WM_USER + 0x103;
        constexpr UINT hasDataMessage = WM_USER + 0x104;
        constexpr UINT receiveSnapshotMessage = WM_USER + 0x105;
        constexpr UINT_PTR armTimer = 1;
        constexpr UINT_PTR externalDragPollTimer = 2;
        constexpr UINT_PTR extractionSweepTimer = 3;
        constexpr UINT armTimeoutMilliseconds = 1000;
        constexpr int startupPollAttempts = 200;
        constexpr DWORD startupPollMilliseconds = 5;
        const ToolWindowCalls systemCalls{CreateThread,
                                          Sleep,
                                          CreateEventW,
                                          CreateWindowExW,
                                          SendMessageW,
                                          IsWindowVisible,
                                          SetWindowPos,
                                          ShowWindow,
                                          SetCapture,
                                          ReleaseCapture,
                                          SetTimer,
                                          KillTimer,
                                          GetWindow,
                                          CoWaitForMultipleHandles,
                                          std::chrono::steady_clock::now};

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
            std::vector<std::wstring> paths;
            core::Button button{core::Button::Left};
            bool needsExtraction{};
            core::DropContext context;
        };

        enum class Mode : std::uint8_t
        {
            Idle,
            SourcePrepared,
            SourceArmed,
            SourceDragging,
            ReceivePending,
            ReceiveArmed,
            ReceiveEntered,
            ReceiveDropping,
            ReceiveRejected
        };

        [[nodiscard]] UINT milliseconds(std::chrono::milliseconds duration)
        {
            return static_cast<UINT>(duration.count());
        }

    } // namespace

    class ToolWindow::State final : public IReceiveLifecycle
    {
      public:
        State(core::IScreen &screen, core::IInput &input, core::IShell &shell, core::IDropData &dropData,
              core::IDropSession &dropSession, core::IExtraction &extraction, core::IFiles &files,
              core::IWindowProperties &properties, core::IDropMenu &menu, const ToolWindowCalls &calls)
            : screen_{screen}, input_{input}, shell_{shell}, dropSession_{dropSession}, extraction_{extraction},
              files_{files}, properties_{properties}, calls_{calls}, dropTarget_{dropSession_, dropData, menu, *this}
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
                // exits keeps the state and COM data alive throughout that unwind.
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
            {
                const std::lock_guard lock{privateMessageMutex_};
                preparePayload_.emplace(PreparePayload{std::vector<std::wstring>{paths.begin(), paths.end()}, button,
                                                       needsExtraction, std::move(context)});
            }
            const auto result = calls_.sendMessage(windowHandle(), prepareDragMessage, 0, 0);
            {
                const std::lock_guard lock{privateMessageMutex_};
                preparePayload_.reset();
            }
            return result != 0;
        }

        [[nodiscard]] bool showAndArm()
        {
            return sendPrivateMessage(startDragMessage, startRequested_) != 0;
        }

        void abort()
        {
            static_cast<void>(sendPrivateMessage(abortDragMessage, abortRequested_));
        }

        [[nodiscard]] bool active() const
        {
            return mode_.load() != Mode::Idle;
        }

        [[nodiscard]] core::NativeWindow nativeWindow() const
        {
            return window_.load();
        }

        [[nodiscard]] bool hasData() const
        {
            return sendPrivateMessage(hasDataMessage, hasDataRequested_) != 0;
        }

        void drop(core::Point point, bool shift)
        {
            constexpr DWORD keyStates[]{0, MK_SHIFT};
            DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
            static_cast<void>(dropTarget_.Drop(nullptr, keyStates[static_cast<std::size_t>(shift)],
                                               POINTL{point.x, point.y}, &effect));
        }

        [[nodiscard]] std::uint32_t dragEnter(std::uintptr_t dataObject, std::uint32_t keyState, core::Point point,
                                              std::uint32_t allowedEffects)
        {
            DWORD effect = allowedEffects;
            static_cast<void>(dropTarget_.DragEnter(reinterpret_cast<IDataObject *>(dataObject), keyState,
                                                    POINTL{point.x, point.y}, &effect));
            return effect;
        }

        void dragLeave()
        {
            static_cast<void>(dropTarget_.DragLeave());
        }

        [[nodiscard]] std::uint32_t drop(std::uintptr_t dataObject, std::uint32_t keyState, core::Point point,
                                         std::uint32_t allowedEffects)
        {
            DWORD effect = allowedEffects;
            static_cast<void>(dropTarget_.Drop(reinterpret_cast<IDataObject *>(dataObject), keyState,
                                               POINTL{point.x, point.y}, &effect));
            return effect;
        }

        void receiveSnapshot(core::ReceiveSnapshot snapshot)
        {
            {
                const std::lock_guard lock{privateMessageMutex_};
                receivePayload_ = std::move(snapshot);
            }
            static_cast<void>(calls_.sendMessage(windowHandle(), receiveSnapshotMessage, 0, 0));
            const std::lock_guard lock{privateMessageMutex_};
            receivePayload_.reset();
        }

        [[nodiscard]] bool receiveMode() const noexcept override
        {
            const auto mode = mode_.load();
            return mode == Mode::ReceiveArmed || mode == Mode::ReceiveEntered || mode == Mode::ReceiveDropping;
        }

        [[nodiscard]] bool sourceMode() const noexcept override
        {
            // The same-Far replay is legitimate only inside our running OLE drag; the arm state precedes the loop
            // and Idle follows it, so a Drop that arrives in either is refused.
            return mode_.load() == Mode::SourceDragging;
        }

        [[nodiscard]] bool enterReceive() override
        {
            auto armed = Mode::ReceiveArmed;
            receiveReleaseDeadline_.reset();
            return mode_.compare_exchange_strong(armed, Mode::ReceiveEntered);
        }

        void leaveReceive() override
        {
            // The release deadline belongs to Entered alone: Armed hides on the next button-up poll instead, and
            // enterReceive starts a fresh one, so nothing depends on whether the exchange took place.
            auto entered = Mode::ReceiveEntered;
            static_cast<void>(mode_.compare_exchange_strong(entered, Mode::ReceiveArmed));
            receiveReleaseDeadline_.reset();
        }

        [[nodiscard]] bool beginReceiveDrop() override
        {
            auto entered = Mode::ReceiveEntered;
            receiveReleaseDeadline_.reset();
            return mode_.compare_exchange_strong(entered, Mode::ReceiveDropping);
        }

        [[nodiscard]] bool refreshReceive(core::Point point) override
        {
            {
                const std::lock_guard lock{privateMessageMutex_};
                receiveRefreshResult_.reset();
            }
            static_cast<void>(ResetEvent(readiness_.get()));
            if (!dropSession_.requestReceiveRefresh(point)) {
                return false;
            }
            HANDLE handle = readiness_.get();
            DWORD signalled{};
            constexpr DWORD pumpFlags =
                static_cast<DWORD>(COWAIT_DISPATCH_CALLS) | static_cast<DWORD>(COWAIT_DISPATCH_WINDOW_MESSAGES);
            const auto status =
                calls_.coWait(pumpFlags, milliseconds(core::receiveDropTimeout), 1, &handle, &signalled);
            if (status != S_OK || signalled != 0) {
                return false;
            }
            const std::lock_guard lock{privateMessageMutex_};
            return std::exchange(receiveRefreshResult_, std::nullopt).value_or(false);
        }

        void completeReceiveRefresh(bool matches)
        {
            {
                const std::lock_guard lock{privateMessageMutex_};
                receiveRefreshResult_ = matches;
            }
            static_cast<void>(SetEvent(readiness_.get()));
        }

        [[nodiscard]] core::NativeWindow menuOwner() const noexcept override
        {
            return window_.load();
        }

        void finishReceive() noexcept override
        {
            calls_.showWindow(windowHandle(), SW_HIDE);
            dropSession_.cancelReceive();
            trackedButton_.reset();
            receiveReleaseDeadline_.reset();
            mode_.store(Mode::Idle);
        }

      private:
        [[nodiscard]] HWND windowHandle() const
        {
            return reinterpret_cast<HWND>(window_.load());
        }

        [[nodiscard]] LRESULT sendPrivateMessage(UINT message, bool &request) const
        {
            {
                const std::lock_guard lock{privateMessageMutex_};
                request = true;
            }
            const auto result = calls_.sendMessage(windowHandle(), message, 0, 0);
            {
                const std::lock_guard lock{privateMessageMutex_};
                request = false;
            }
            return result;
        }

        [[nodiscard]] bool takePrivateRequest(bool &request)
        {
            const std::lock_guard lock{privateMessageMutex_};
            return std::exchange(request, false);
        }

        [[nodiscard]] std::optional<PreparePayload> takePreparePayload()
        {
            const std::lock_guard lock{privateMessageMutex_};
            return std::exchange(preparePayload_, std::nullopt);
        }

        [[nodiscard]] std::optional<core::ReceiveSnapshot> takeReceivePayload()
        {
            const std::lock_guard lock{privateMessageMutex_};
            return std::exchange(receivePayload_, std::nullopt);
        }

        [[nodiscard]] static bool validPrivateMessageParameters(WPARAM word, LPARAM number)
        {
            return word == 0 && number == 0;
        }

        static DWORD WINAPI threadEntry(void *parameter)
        {
            return static_cast<State *>(parameter)->threadMain();
        }

        [[nodiscard]] DWORD threadMain()
        {
            MSG message{};
            static_cast<void>(PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE));
            static_cast<void>(OleInitialize(nullptr));
            if (stopRequested_.load()) {
                static_cast<void>(SetEvent(readiness_.get()));
                OleUninitialize();
                return 0;
            }

            // The class is registered under the module that holds windowProcedure, not under the host executable:
            // Far can unload a plugin DLL, and a class left under far.exe's handle with a procedure in the unloaded
            // image makes the next CreateWindowExW of that name fast-fail in USER32's control-flow-guard check
            // (observed in the e2e process, which unloads Burlak.dll between tests). The same module unregisters
            // it below when the last window is gone.
            HMODULE module{};
            static_cast<void>(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                     GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                 reinterpret_cast<LPCWSTR>(&windowProcedure), &module));
            WNDCLASSEXW windowClass{};
            windowClass.cbSize = sizeof(windowClass);
            windowClass.lpfnWndProc = windowProcedure;
            windowClass.hInstance = module;
            windowClass.lpszClassName = toolClass;
            bool registered = RegisterClassExW(&windowClass) != 0;
            if (!registered) {
                // Another instance in this process (the tests run several) has already registered this very class;
                // any other class of the same name under this module is foreign and fails start() instead of being
                // adopted with its procedure. A failed lookup leaves the zeroed record's procedure null, which is
                // refused the same way.
                WNDCLASSEXW existing{};
                existing.cbSize = sizeof(existing);
                static_cast<void>(GetClassInfoExW(module, toolClass, &existing));
                registered = existing.lpfnWndProc == windowProcedure;
            }

            const auto owner = reinterpret_cast<HWND>(screen_.hostWindowHandle());
            UniqueWindow ownedWindow{
                registered ? calls_.createWindow(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, toolClass,
                                                 nullptr, WS_POPUP, 0, 0, 1, 1, owner, nullptr, module, this)
                           : nullptr};
            const auto window = ownedWindow.get();
            DragDropRegistration registration;
            if (window != nullptr) {
                static_cast<void>(SetLayeredWindowAttributes(window, 0, 1, LWA_ALPHA));
                window_.store(reinterpret_cast<core::NativeWindow>(window));
                static_cast<void>(RegisterDragDrop(window, &dropTarget_));
                registration.reset(window);
                calls_.setTimer(window, externalDragPollTimer, milliseconds(core::externalDragPollInterval), nullptr);
                calls_.setTimer(window, extractionSweepTimer, milliseconds(core::extractionSweepInterval), nullptr);
            }
            static_cast<void>(SetEvent(readiness_.get()));

            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                static_cast<void>(TranslateMessage(&message));
                static_cast<void>(DispatchMessageW(&message));
            }

            calls_.killTimer(window, externalDragPollTimer);
            calls_.killTimer(window, extractionSweepTimer);
            clearForShutdown();
            registration.reset();
            ownedWindow.reset();
            window_.store(0);
            if (registered) {
                // Fails harmlessly while another instance still has a window of this class; the last one succeeds.
                static_cast<void>(UnregisterClassW(toolClass, module));
            }
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
            switch (message) {
            case prepareDragMessage:
                return prepareSource(validPrivateMessageParameters(word, number));
            case startDragMessage:
                return validPrivateMessageParameters(word, number) && takePrivateRequest(startRequested_)
                           ? showAndArm(window)
                           : 0;
            case abortDragMessage:
                if (validPrivateMessageParameters(word, number) && takePrivateRequest(abortRequested_)) {
                    abortCurrent();
                }
                return 0;
            case hasDataMessage:
                return validPrivateMessageParameters(word, number) && takePrivateRequest(hasDataRequested_) && data_
                           ? 1
                           : 0;
            case receiveSnapshotMessage:
                return acceptReceiveSnapshot(window, validPrivateMessageParameters(word, number));
            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN:
                calls_.killTimer(window, armTimer);
                runDrag(window);
                return 0;
            case WM_TIMER:
                handleTimer(window, word);
                return 0;
            case WM_DESTROY:
                calls_.showWindow(window, SW_HIDE);
                return 0;
            default:
                return DefWindowProcW(window, message, word, number);
            }
        }

        [[nodiscard]] LRESULT prepareSource(bool validParameters)
        {
            if (!validParameters) {
                return 0;
            }
            auto payload = takePreparePayload();
            if (!payload || mode_.load() != Mode::Idle) {
                return 0;
            }
            auto prepared = shell_.makeDataObject(payload->paths, core::preferredDropEffect(payload->needsExtraction));
            // Far's eventual copy consumes its live selection, so every selected path must also be advertised
            // (Far source: far/filelist.cpp, FileList::ProcessCopyKeys).
            if (!prepared || !core::allPathsAdvertised(payload->paths.size(), prepared->parsedPaths)) {
                return 0;
            }
            button_ = payload->button;
            needsExtraction_ = payload->needsExtraction;
            dropSession_.prepare(std::move(payload->context));
            data_ = std::move(prepared->data);
            mode_.store(Mode::SourcePrepared);
            return 1;
        }

        [[nodiscard]] LRESULT showAndArm(HWND window)
        {
            if (mode_.load() != Mode::SourcePrepared) {
                return 0;
            }
            const auto host = screen_.hostWindow();
            if (!host) {
                clearSource();
                mode_.store(Mode::Idle);
                return 0;
            }
            position(window, *host, host->rect);
            if (!calls_.isWindowVisible(window)) {
                clearSource();
                mode_.store(Mode::Idle);
                return 0;
            }
            mode_.store(Mode::SourceArmed);
            calls_.setCapture(window);
            calls_.setTimer(window, armTimer, armTimeoutMilliseconds, nullptr);
            // Session released the physical button; this press is a new click on the tool window.
            input_.press(button_);
            return 1;
        }

        void position(HWND window, const core::HostWindow &host, core::PixelRect rectangle)
        {
            const auto choice = core::placement(host.topmost);
            if (choice.demoteFirst) {
                calls_.setWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            const auto nativeHost = reinterpret_cast<HWND>(host.handle);
            auto insertAfter = calls_.getWindow(nativeHost, GW_HWNDPREV);
            if (insertAfter == window) {
                insertAfter = calls_.getWindow(window, GW_HWNDPREV);
            }
            if (insertAfter == nullptr) {
                insertAfter = choice.topmost ? HWND_TOPMOST : HWND_TOP;
            }
            calls_.setWindowPos(window, insertAfter, rectangle.left, rectangle.top, rectangle.right - rectangle.left,
                                rectangle.bottom - rectangle.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }

        void runDrag(HWND window)
        {
            auto expected = Mode::SourceArmed;
            if (!data_ || !mode_.compare_exchange_strong(expected, Mode::SourceDragging)) {
                return;
            }
            DragSource source{
                releasePolicy_,  screen_, extraction_, button_, reinterpret_cast<core::NativeWindow>(window),
                needsExtraction_};
            const auto outcome = shell_.runDrag(reinterpret_cast<core::NativeWindow>(window), *data_,
                                                reinterpret_cast<std::uintptr_t>(&source), !needsExtraction_);
            calls_.releaseCapture();
            calls_.showWindow(window, SW_HIDE);
            // Retention or cleanup completes before Idle is published, so neither a source gesture nor timer sweep
            // can race the run that just left OLE.
            clearSource(core::retainExtractedRun(source.extractionRan(), outcome, DRAGDROP_S_DROP));
            mode_.store(Mode::Idle);
        }

        void disarm(HWND window)
        {
            calls_.killTimer(window, armTimer);
            if (mode_.load() != Mode::SourceArmed) {
                return;
            }
            calls_.releaseCapture();
            calls_.showWindow(window, SW_HIDE);
            clearSource();
            mode_.store(Mode::Idle);
        }

        void clearSource(bool preserveExtraction = false)
        {
            data_.reset();
            if (std::exchange(needsExtraction_, false)) {
                if (preserveExtraction) {
                    extraction_.retain();
                } else {
                    extraction_.cleanup();
                }
            }
            // Drop the hover context the moment the source drag ends, so no later call can build a same-Far replay
            // from a stale selection. The Far-thread drop/identity snapshot survives here because a same-Far release
            // may still have a redraw synchro in flight; it is inert without the hover context and the next begin
            // rebuilds it.
            dropSession_.endSource();
        }

        void abortCurrent()
        {
            const auto mode = mode_.load();
            if (mode == Mode::ReceiveArmed || mode == Mode::ReceiveEntered || mode == Mode::ReceiveDropping ||
                mode == Mode::ReceivePending || mode == Mode::ReceiveRejected) {
                finishReceive();
                return;
            }
            clearSource();
            mode_.store(Mode::Idle);
        }

        void clearForShutdown()
        {
            dropSession_.cancelReceive();
            clearSource();
            trackedButton_.reset();
            receiveReleaseDeadline_.reset();
            mode_.store(Mode::Idle);
        }

        void handleTimer(HWND window, WPARAM timer)
        {
            if (timer == armTimer) {
                disarm(window);
            } else if (timer == externalDragPollTimer) {
                pollExternalDrag();
            } else if (timer == extractionSweepTimer && mode_.load() == Mode::Idle) {
                files_.sweep();
            }
        }

        void pollExternalDrag()
        {
            const auto mode = mode_.load();
            if (mode == Mode::SourcePrepared || mode == Mode::SourceArmed || mode == Mode::SourceDragging) {
                return;
            }
            if (mode == Mode::ReceiveDropping) {
                return;
            }
            const bool leftDown = screen_.buttonDown(core::Button::Left);
            const bool rightDown = screen_.buttonDown(core::Button::Right);
            if (trackedButton_) {
                const bool held = *trackedButton_ == core::Button::Left ? leftDown : rightDown;
                if (!held) {
                    const auto releaseMode = mode_.load();
                    if (releaseMode == Mode::ReceiveEntered) {
                        if (!receiveReleaseDeadline_) {
                            receiveReleaseDeadline_ = calls_.now() + core::receiveEnteredTimeout;
                        } else if (calls_.now() >= *receiveReleaseDeadline_) {
                            finishReceive();
                        }
                    } else if (releaseMode == Mode::ReceiveArmed || releaseMode == Mode::ReceivePending ||
                               releaseMode == Mode::ReceiveRejected) {
                        finishReceive();
                    } else {
                        trackedButton_.reset();
                    }
                    return;
                }
            }
            if (mode_.load() != Mode::Idle) {
                return;
            }
            if (!trackedButton_) {
                if (!leftDown && !rightDown) {
                    return;
                }
                const auto press = screen_.cursor();
                if (!press) {
                    return;
                }
                trackedButton_ = rightDown ? core::Button::Right : core::Button::Left;
                pressPoint_ = *press;
                pressRoot_ = screen_.windowAt(*press);
            }
            const auto point = screen_.cursor();
            if (!point) {
                return;
            }
            const auto host = screen_.hostWindowAt(*point);
            const auto hostHandle =
                host.transform([](const core::HostWindow &window) { return window.handle; }).value_or(0);
            const auto receiver = host ? properties_.value(host->handle) : std::nullopt;
            const auto process = properties_.processId();
            const core::ExternalDragFacts facts{
                .buttonDown = *trackedButton_ == core::Button::Left ? leftDown : rightDown,
                .pressRoot = pressRoot_,
                .pointRoot = screen_.windowAt(*point),
                .host = hostHandle,
                .console = screen_.consoleWindow(),
                .tool = window_.load(),
                .receiver = receiver,
                .receiverAlive = !receiver || *receiver == process || files_.processAlive(*receiver),
                .process = process,
                .ownDragActive = false};
            if (!externalDragPolicy_.overHost(facts)) {
                return;
            }
            // A drag-selection from another program can cross Far and satisfy these physical facts. Its capture
            // keeps OLE away from this window; the invisible overlay is discarded when the button rises.
            mode_.store(Mode::ReceivePending);
            if (!dropSession_.requestReceiveSnapshot(*point)) {
                mode_.store(Mode::ReceiveRejected);
            }
        }

        [[nodiscard]] LRESULT acceptReceiveSnapshot(HWND window, bool validParameters)
        {
            if (!validParameters) {
                return 0;
            }
            auto snapshot = takeReceivePayload();
            if (!snapshot || mode_.load() != Mode::ReceivePending) {
                return 0;
            }
            const auto rectangle = receivePolicy_.overlayRect(*snapshot);
            if (!rectangle) {
                mode_.store(Mode::ReceiveRejected);
                return 0;
            }
            dropSession_.prepareReceive(*snapshot);
            position(window, *snapshot->host, *rectangle);
            if (!calls_.isWindowVisible(window)) {
                dropSession_.cancelReceive();
                mode_.store(Mode::ReceiveRejected);
                return 0;
            }
            mode_.store(Mode::ReceiveArmed);
            return 1;
        }

        core::IScreen &screen_;
        core::IInput &input_;
        core::IShell &shell_;
        core::IDropSession &dropSession_;
        core::IExtraction &extraction_;
        core::IFiles &files_;
        core::IWindowProperties &properties_;
        const ToolWindowCalls &calls_;
        UniqueHandle thread_;
        UniqueHandle readiness_;
        DWORD threadId_{};
        std::atomic<core::NativeWindow> window_{};
        std::atomic<Mode> mode_{Mode::Idle};
        std::atomic<bool> stopRequested_{};
        mutable std::mutex privateMessageMutex_;
        std::optional<PreparePayload> preparePayload_;
        std::optional<core::ReceiveSnapshot> receivePayload_;
        std::optional<bool> receiveRefreshResult_;
        mutable bool startRequested_{};
        mutable bool abortRequested_{};
        mutable bool hasDataRequested_{};
        core::Button button_{core::Button::Left};
        bool needsExtraction_{};
        std::unique_ptr<core::IShell::DragData> data_;
        std::optional<core::Button> trackedButton_;
        std::optional<std::chrono::steady_clock::time_point> receiveReleaseDeadline_;
        core::Point pressPoint_{};
        core::NativeWindow pressRoot_{};
        core::ReleasePolicy releasePolicy_;
        core::ExternalDragPolicy externalDragPolicy_;
        core::ReceivePolicy receivePolicy_;
        DropTarget dropTarget_;
    };

    ToolWindow::ToolWindow(core::IScreen &screen, core::IInput &input, core::IShell &shell, core::IDropData &dropData,
                           core::IDropSession &dropSession, core::IExtraction &extraction, core::IFiles &files,
                           core::IWindowProperties &properties, core::IDropMenu &menu)
        : state_{std::make_unique<State>(screen, input, shell, dropData, dropSession, extraction, files, properties,
                                         menu, systemCalls)}
    {
    }

    ToolWindow::ToolWindow(core::IScreen &screen, core::IInput &input, core::IShell &shell, core::IDropData &dropData,
                           core::IDropSession &dropSession, core::IExtraction &extraction, core::IFiles &files,
                           core::IWindowProperties &properties, core::IDropMenu &menu, const ToolWindowCalls &calls)
        : state_{std::make_unique<State>(screen, input, shell, dropData, dropSession, extraction, files, properties,
                                         menu, calls)}
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
        return state_->prepare(paths, button, needsExtraction, std::move(context));
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

    void ToolWindow::drop(core::Point point, bool shift)
    {
        state_->drop(point, shift);
    }

    std::uint32_t ToolWindow::dragEnter(std::uintptr_t dataObject, std::uint32_t keyState, core::Point point,
                                        std::uint32_t allowedEffects)
    {
        return state_->dragEnter(dataObject, keyState, point, allowedEffects);
    }

    void ToolWindow::dragLeave()
    {
        state_->dragLeave();
    }

    std::uint32_t ToolWindow::drop(std::uintptr_t dataObject, std::uint32_t keyState, core::Point point,
                                   std::uint32_t allowedEffects)
    {
        return state_->drop(dataObject, keyState, point, allowedEffects);
    }

    void ToolWindow::receiveSnapshot(core::ReceiveSnapshot snapshot)
    {
        state_->receiveSnapshot(std::move(snapshot));
    }

    void ToolWindow::completeReceiveRefresh(bool matches)
    {
        state_->completeReceiveRefresh(matches);
    }

    std::uintptr_t armTimerId()
    {
        return armTimer;
    }

    std::uintptr_t externalDragPollTimerId()
    {
        return externalDragPollTimer;
    }

    std::uintptr_t extractionSweepTimerId()
    {
        return extractionSweepTimer;
    }

    const ToolWindowCalls &systemToolWindowCalls()
    {
        return systemCalls;
    }

} // namespace burlak::drag
