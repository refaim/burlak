#pragma once

#include "core/Interfaces.hpp"

#include <windows.h>

#include <chrono>
#include <memory>

namespace burlak::drag
{

    struct ToolWindowCalls
    {
        decltype(&CreateThread) createThread;
        decltype(&Sleep) sleep;
        decltype(&CreateEventW) createEvent;
        decltype(&CreateWindowExW) createWindow;
        decltype(&SendMessageW) sendMessage;
        decltype(&IsWindowVisible) isWindowVisible;
        decltype(&SetWindowPos) setWindowPos;
        decltype(&ShowWindow) showWindow;
        decltype(&SetCapture) setCapture;
        decltype(&ReleaseCapture) releaseCapture;
        decltype(&SetTimer) setTimer;
        decltype(&KillTimer) killTimer;
        decltype(&GetWindow) getWindow;
        decltype(&CoWaitForMultipleHandles) coWait;
        std::chrono::steady_clock::time_point (*now)();
    };

    class ToolWindow final : public core::IDragTool
    {
      public:
        ToolWindow(core::IScreen &screen, core::IInput &input, core::IShell &shell, core::IDropData &dropData,
                   core::IDropSession &dropSession, core::IExtraction &extraction, core::IFiles &files,
                   core::IWindowProperties &properties, core::IDropMenu &menu);
        ToolWindow(core::IScreen &screen, core::IInput &input, core::IShell &shell, core::IDropData &dropData,
                   core::IDropSession &dropSession, core::IExtraction &extraction, core::IFiles &files,
                   core::IWindowProperties &properties, core::IDropMenu &menu, const ToolWindowCalls &calls);
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
        void drop(core::Point point, bool shift);
        [[nodiscard]] std::uint32_t dragEnter(std::uintptr_t dataObject, std::uint32_t keyState, core::Point point,
                                              std::uint32_t allowedEffects);
        void dragLeave();
        [[nodiscard]] std::uint32_t drop(std::uintptr_t dataObject, std::uint32_t keyState, core::Point point,
                                         std::uint32_t allowedEffects);
        void receiveSnapshot(core::ReceiveSnapshot snapshot);
        void completeReceiveRefresh(bool matches);

      private:
        class State;
        std::unique_ptr<State> state_;
    };

    [[nodiscard]] std::uintptr_t armTimerId();
    [[nodiscard]] std::uintptr_t externalDragPollTimerId();
    [[nodiscard]] std::uintptr_t extractionSweepTimerId();
    [[nodiscard]] const ToolWindowCalls &systemToolWindowCalls();

} // namespace burlak::drag
