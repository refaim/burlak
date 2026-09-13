#include "adapters/win/Screen.hpp"

#include <windows.h>

#include <array>

namespace burlak::adapters::win
{

    namespace
    {

        const ScreenCalls systemCalls{GetCursorPos,          GetAsyncKeyState,  GetConsoleWindow,
                                      GetForegroundWindow,   GetWindowRect,     GetClientRect,
                                      IsWindowVisible,       GetWindowLongPtrW, GetStdHandle,
                                      GetCurrentConsoleFont, ClientToScreen,    GetConsoleScreenBufferInfo};

        [[nodiscard]] std::optional<core::HostWindow> coveringWindow(core::NativeWindow native, core::Point point,
                                                                     const ScreenCalls &calls)
        {
            const auto window = reinterpret_cast<HWND>(native);
            RECT rect{};
            if (window == nullptr || !calls.getWindowRect(window, &rect) || !calls.isWindowVisible(window)) {
                return std::nullopt;
            }
            const POINT cursor{point.x, point.y};
            if (!PtInRect(&rect, cursor)) {
                return std::nullopt;
            }
            return core::HostWindow{.handle = native,
                                    .rect = {rect.left, rect.top, rect.right, rect.bottom},
                                    .topmost = (calls.getWindowLong(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0};
        }

    } // namespace

    Screen::Screen() : calls_{systemCalls}
    {
    }

    Screen::Screen(const ScreenCalls &calls) : calls_{calls}
    {
    }

    std::optional<core::Point> Screen::cursor()
    {
        POINT point{};
        if (!calls_.getCursorPos(&point)) {
            return std::nullopt;
        }
        return core::Point{point.x, point.y};
    }

    bool Screen::buttonDown(core::Button button)
    {
        constexpr std::array<int, 2> virtualKeys{VK_LBUTTON, VK_RBUTTON};
        return (calls_.getAsyncKeyState(virtualKeys.at(static_cast<std::size_t>(button))) & 0x8000) != 0;
    }

    std::optional<core::HostWindow> Screen::hostWindow()
    {
        const auto point = cursor();
        if (!point) {
            return std::nullopt;
        }
        return hostWindowAt(*point, reinterpret_cast<core::NativeWindow>(calls_.getConsoleWindow()),
                            reinterpret_cast<core::NativeWindow>(calls_.getForegroundWindow()), calls_);
    }

    std::expected<core::CellGeometry, core::Error> Screen::cellGeometry()
    {
        const auto output = calls_.getStdHandle(STD_OUTPUT_HANDLE);
        const auto host = hostWindow();
        const auto console = calls_.getConsoleWindow();
        CONSOLE_FONT_INFO font{};
        POINT fontOrigin{};
        if (output != INVALID_HANDLE_VALUE && host && host->handle == reinterpret_cast<core::NativeWindow>(console) &&
            calls_.getCurrentConsoleFont(output, FALSE, &font) && font.dwFontSize.X > 0 && font.dwFontSize.Y > 0 &&
            calls_.clientToScreen(console, &fontOrigin)) {
            return core::CellGeometry{.origin = {fontOrigin.x, fontOrigin.y},
                                      .cellWidth = font.dwFontSize.X,
                                      .cellHeight = font.dwFontSize.Y};
        }

        CONSOLE_SCREEN_BUFFER_INFO buffer{};
        if (output == INVALID_HANDLE_VALUE || !calls_.getConsoleScreenBufferInfo(output, &buffer) || !host) {
            return std::unexpected(core::Error::Unavailable);
        }
        const int columns = buffer.srWindow.Right - buffer.srWindow.Left + 1;
        const int rows = buffer.srWindow.Bottom - buffer.srWindow.Top + 1;
        if (columns <= 0 || rows <= 0) {
            return std::unexpected(core::Error::Unavailable);
        }
        const auto hostWindow = reinterpret_cast<HWND>(host->handle);
        RECT client{};
        if (!calls_.getClientRect(hostWindow, &client)) {
            return std::unexpected(core::Error::Unavailable);
        }
        POINT origin{client.left, client.top};
        if (!calls_.clientToScreen(hostWindow, &origin)) {
            return std::unexpected(core::Error::Unavailable);
        }
        const int width = (client.right - client.left) / columns;
        const int height = (client.bottom - client.top) / rows;
        if (width <= 0 || height <= 0) {
            return std::unexpected(core::Error::Unavailable);
        }
        return core::CellGeometry{.origin = {origin.x, origin.y}, .cellWidth = width, .cellHeight = height};
    }

    std::optional<core::HostWindow> hostWindowAt(core::Point point, core::NativeWindow console,
                                                 core::NativeWindow foreground)
    {
        return hostWindowAt(point, console, foreground, systemCalls);
    }

    std::optional<core::HostWindow> hostWindowAt(core::Point point, core::NativeWindow console,
                                                 core::NativeWindow foreground, const ScreenCalls &calls)
    {
        if (const auto covered = coveringWindow(console, point, calls)) {
            return covered;
        }
        return coveringWindow(foreground, point, calls);
    }

} // namespace burlak::adapters::win
