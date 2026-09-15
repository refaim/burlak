#pragma once

#include "core/Interfaces.hpp"

#include <windows.h>

namespace burlak::adapters::win
{

    struct DropMenuCalls
    {
        decltype(&CreatePopupMenu) createMenu;
        decltype(&AppendMenuW) appendMenu;
        decltype(&TrackPopupMenu) trackMenu;
        decltype(&DestroyMenu) destroyMenu;
        decltype(&SetForegroundWindow) setForegroundWindow;
        decltype(&GetForegroundWindow) getForegroundWindow;
        decltype(&GetWindowThreadProcessId) getWindowThread;
        decltype(&GetCurrentThreadId) getCurrentThreadId;
        decltype(&AttachThreadInput) attachInput;
        decltype(&PostMessageW) postMessage;
    };

    class DropMenu final : public core::IDropMenu
    {
      public:
        DropMenu();
        explicit DropMenu(const DropMenuCalls &calls);

        [[nodiscard]] core::DropMenuChoice choose(core::NativeWindow owner, core::Point point,
                                                  core::AllowedEffects allowed = {.copy = true, .move = true}) override;

      private:
        const DropMenuCalls &calls_;
    };

    [[nodiscard]] const DropMenuCalls &systemDropMenuCalls();

} // namespace burlak::adapters::win
