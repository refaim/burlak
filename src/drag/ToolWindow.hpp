#pragma once

#include "core/Interfaces.hpp"

#include <windows.h>

#include <memory>

namespace burlak::drag
{

    struct ToolWindowCalls
    {
        decltype(&CreateThread) createThread;
        decltype(&Sleep) sleep;
        decltype(&CreateEventW) createEvent;
        decltype(&CreateWindowExW) createWindow;
        decltype(&IsWindowVisible) isWindowVisible;
        decltype(&SetWindowPos) setWindowPos;
        decltype(&ShowWindow) showWindow;
        decltype(&SetCapture) setCapture;
        decltype(&ReleaseCapture) releaseCapture;
        decltype(&SetTimer) setTimer;
        decltype(&KillTimer) killTimer;
        decltype(&CoWaitForMultipleHandles) coWait;
    };

    class ToolWindow final : public core::IDragTool
    {
      public:
        ToolWindow(core::IScreen &screen, core::IInput &input, core::IShell &shell, core::IDropSession &dropSession,
                   core::IExtraction &extraction);
        ToolWindow(core::IScreen &screen, core::IInput &input, core::IShell &shell, core::IDropSession &dropSession,
                   core::IExtraction &extraction, const ToolWindowCalls &calls);
        ~ToolWindow();

        [[nodiscard]] bool start() override;
        [[nodiscard]] bool prepare(std::span<const std::wstring> paths, core::Button button, bool needsExtraction,
                                   core::DropContext context) override;
        [[nodiscard]] bool showAndArm() override;
        void abort() override;
        [[nodiscard]] bool active() const override;
        void stop() override;

        [[nodiscard]] core::NativeWindow nativeWindow() const;
        [[nodiscard]] bool hasData() const;

      private:
        class State;
        std::unique_ptr<State> state_;
    };

    [[nodiscard]] std::uintptr_t armTimerId();
    [[nodiscard]] const ToolWindowCalls &systemToolWindowCalls();

} // namespace burlak::drag
