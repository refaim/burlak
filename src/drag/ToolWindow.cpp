#include "drag/ToolWindow.hpp"

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
        constexpr wchar_t toolClass[] = L"BurlakToolWindow";

        const ToolWindowCalls systemCalls{CreateThread, Sleep,      CreateWindowExW, IsWindowVisible, SetWindowPos,
                                          ShowWindow,   SetCapture, ReleaseCapture,  SetTimer,        KillTimer};

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
            core::DropContext context;
        };

    } // namespace

    class ToolWindow::State
    {
      public:
        State(core::IScreen &screen, core::IInput &input, core::IShell &shell, core::IDropSession &dropSession,
              const ToolWindowCalls &calls)
            : screen_{screen}, input_{input}, shell_{shell}, dropSession_{dropSession}, calls_{calls},
              dropTarget_{dropSession_}
        {
        }

        [[nodiscard]] bool start()
        {
            if (thread_.get() != nullptr) {
                return window_.load() != 0;
            }
            thread_.reset(calls_.createThread(nullptr, 0, threadEntry, this, 0, &threadId_));
            if (thread_.get() == nullptr) {
                return false;
            }
            for (int attempt = 0; attempt < 200 && window_.load() == 0; ++attempt) {
                calls_.sleep(5);
            }
            return window_.load() != 0;
        }

        void stop()
        {
            if (threadId_ != 0) {
                static_cast<void>(PostThreadMessageW(threadId_, WM_QUIT, 0, 0));
            }
            if (thread_.get() != nullptr) {
                static_cast<void>(WaitForSingleObject(thread_.get(), 3000));
                thread_.reset();
            }
            threadId_ = 0;
        }

        [[nodiscard]] bool prepare(std::span<const std::wstring> paths, core::Button button, core::DropContext context)
        {
            // SendMessage is synchronous across these threads, so the path view remains alive while the
            // context value transfers the Far-thread snapshot before any hover or drop can read it.
            const PreparePayload payload{paths, button, std::move(context)};
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
            static_cast<void>(OleInitialize(nullptr));

            WNDCLASSEXW windowClass{};
            windowClass.cbSize = sizeof(windowClass);
            windowClass.lpfnWndProc = windowProcedure;
            windowClass.hInstance = GetModuleHandleW(nullptr);
            windowClass.lpszClassName = toolClass;
            static_cast<void>(RegisterClassExW(&windowClass));

            UniqueWindow ownedWindow{calls_.createWindow(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, toolClass,
                                                         nullptr, WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                                         windowClass.hInstance, this)};
            const auto window = ownedWindow.get();
            DragDropRegistration registration;
            if (window != nullptr) {
                static_cast<void>(SetLayeredWindowAttributes(window, 0, 1, LWA_ALPHA));
                window_.store(reinterpret_cast<core::NativeWindow>(window));
                static_cast<void>(RegisterDragDrop(window, &dropTarget_));
                registration.reset(window);
            }

            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                static_cast<void>(TranslateMessage(&message));
                static_cast<void>(DispatchMessageW(&message));
            }

            data_.reset();
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
                return state->handleMessage(window, message, word, number);
            }
            return DefWindowProcW(window, message, word, number);
        }

        LRESULT handleMessage(HWND window, UINT message, WPARAM word, LPARAM number)
        {
            switch (message) {
            case prepareDragMessage: {
                const auto &payload = *reinterpret_cast<const PreparePayload *>(number);
                data_.reset();
                auto prepared = shell_.makeDataObject(payload.paths);
                if (!prepared) {
                    return 0;
                }
                button_ = payload.button;
                dropSession_.prepare(payload.context);
                data_ = std::move(*prepared);
                return 1;
            }
            case startDragMessage:
                return showAndArm(window);
            case abortDragMessage:
                data_.reset();
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
                data_.reset();
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
                data_.reset();
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
            DragSource source{releasePolicy_, button_};
            static_cast<void>(shell_.runDrag(reinterpret_cast<core::NativeWindow>(window), *data_,
                                             reinterpret_cast<std::uintptr_t>(&source)));
            active_.store(false);
            calls_.releaseCapture();
            calls_.showWindow(window, SW_HIDE);
            data_.reset();
        }

        void disarm(HWND window)
        {
            calls_.killTimer(window, armTimer);
            if (active_.load()) {
                return;
            }
            calls_.releaseCapture();
            calls_.showWindow(window, SW_HIDE);
            data_.reset();
        }

        core::IScreen &screen_;
        core::IInput &input_;
        core::IShell &shell_;
        core::IDropSession &dropSession_;
        const ToolWindowCalls &calls_;
        UniqueHandle thread_;
        DWORD threadId_{};
        std::atomic<core::NativeWindow> window_{};
        std::atomic<bool> active_{};
        core::Button button_{core::Button::Left};
        std::unique_ptr<core::IShell::DragData> data_;
        core::ReleasePolicy releasePolicy_;
        DropTarget dropTarget_;
    };

    ToolWindow::ToolWindow(core::IScreen &screen, core::IInput &input, core::IShell &shell,
                           core::IDropSession &dropSession)
        : state_{std::make_unique<State>(screen, input, shell, dropSession, systemCalls)}
    {
    }

    ToolWindow::ToolWindow(core::IScreen &screen, core::IInput &input, core::IShell &shell,
                           core::IDropSession &dropSession, const ToolWindowCalls &calls)
        : state_{std::make_unique<State>(screen, input, shell, dropSession, calls)}
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

    bool ToolWindow::prepare(std::span<const std::wstring> paths, core::Button button, core::DropContext context)
    {
        return state_->prepare(paths, button, context);
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
