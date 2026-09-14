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
        decltype(&GetWindow) getWindow;
        decltype(&WindowFromPoint) windowFromPoint;
        decltype(&GetAncestor) getAncestor;
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
        [[nodiscard]] core::NativeWindow windowAt(core::Point point) override;
        [[nodiscard]] core::NativeWindow consoleWindow() override;
        [[nodiscard]] core::NativeWindow hostWindowHandle() override;
        [[nodiscard]] std::optional<core::HostWindow> hostWindow() override;
        [[nodiscard]] std::optional<core::HostWindow> hostWindowAt(core::Point point) override;
        [[nodiscard]] std::expected<core::CellGeometry, core::Error> cellGeometry() override;
        [[nodiscard]] std::expected<core::CellGeometry, core::Error> cellGeometryAt(core::Point point) override;

      private:
        const ScreenCalls &calls_;
    };

    [[nodiscard]] std::optional<core::HostWindow> hostWindowAt(core::Point point, core::NativeWindow console,
                                                               core::NativeWindow owner);
    [[nodiscard]] std::optional<core::HostWindow> hostWindowAt(core::Point point, core::NativeWindow console,
                                                               core::NativeWindow owner, const ScreenCalls &calls);

} // namespace burlak::adapters::win
