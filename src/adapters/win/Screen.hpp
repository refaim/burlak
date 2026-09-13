#pragma once

#include "core/Interfaces.hpp"

#include <windows.h>

namespace burlak::adapters::win
{

    struct ScreenCalls
    {
        decltype(&GetCursorPos) getCursorPos;
        decltype(&GetAsyncKeyState) getAsyncKeyState;
        decltype(&GetConsoleWindow) getConsoleWindow;
        decltype(&GetForegroundWindow) getForegroundWindow;
        decltype(&GetWindowRect) getWindowRect;
        decltype(&GetClientRect) getClientRect;
        decltype(&IsWindowVisible) isWindowVisible;
        decltype(&GetWindowLongPtrW) getWindowLong;
        decltype(&GetStdHandle) getStdHandle;
        decltype(&GetCurrentConsoleFont) getCurrentConsoleFont;
        decltype(&ClientToScreen) clientToScreen;
        decltype(&GetConsoleScreenBufferInfo) getConsoleScreenBufferInfo;
    };

    class Screen final : public core::IScreen
    {
      public:
        Screen();
        explicit Screen(const ScreenCalls &calls);

        [[nodiscard]] std::optional<core::Point> cursor() override;
        [[nodiscard]] bool buttonDown(core::Button button) override;
        [[nodiscard]] std::optional<core::HostWindow> hostWindow() override;
        [[nodiscard]] std::expected<core::CellGeometry, core::Error> cellGeometry() override;

      private:
        const ScreenCalls &calls_;
    };

    [[nodiscard]] std::optional<core::HostWindow> hostWindowAt(core::Point point, core::NativeWindow console,
                                                               core::NativeWindow foreground);
    [[nodiscard]] std::optional<core::HostWindow> hostWindowAt(core::Point point, core::NativeWindow console,
                                                               core::NativeWindow foreground, const ScreenCalls &calls);

} // namespace burlak::adapters::win
