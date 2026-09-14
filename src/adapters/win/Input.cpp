#include "adapters/win/Input.hpp"

#include "core/Policies.hpp"

#include <windows.h>

#include <vector>

namespace burlak::adapters::win
{

    namespace
    {

        const InputCalls systemCalls{mouse_event, GetConsoleScreenBufferInfo, WriteConsoleInputW};

        [[nodiscard]] DWORD mouseFlag(core::Button button, bool down)
        {
            constexpr DWORD flags[2][2]{{MOUSEEVENTF_LEFTUP, MOUSEEVENTF_LEFTDOWN},
                                        {MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_RIGHTDOWN}};
            return flags[static_cast<std::size_t>(button)][down ? 1U : 0U];
        }

        [[nodiscard]] INPUT_RECORD consoleRecord(const core::MouseEvent &event)
        {
            INPUT_RECORD record{};
            record.EventType = MOUSE_EVENT;
            auto &mouse = record.Event.MouseEvent;
            mouse.dwMousePosition = {static_cast<SHORT>(event.at.x), static_cast<SHORT>(event.at.y)};
            mouse.dwButtonState =
                (event.left ? FROM_LEFT_1ST_BUTTON_PRESSED : 0U) | (event.right ? RIGHTMOST_BUTTON_PRESSED : 0U);
            mouse.dwControlKeyState = (event.mods.shift ? SHIFT_PRESSED : 0U) |
                                      (event.mods.control ? LEFT_CTRL_PRESSED : 0U) |
                                      (event.mods.alt ? LEFT_ALT_PRESSED : 0U);
            mouse.dwEventFlags = event.wheel ? MOUSE_WHEELED : (event.moved ? MOUSE_MOVED : 0U);
            return record;
        }

    } // namespace

    Input::Input()
        : Input{reinterpret_cast<core::NativeWindow>(GetStdHandle(STD_INPUT_HANDLE)),
                reinterpret_cast<core::NativeWindow>(GetStdHandle(STD_OUTPUT_HANDLE)), systemCalls}
    {
    }

    Input::Input(core::NativeWindow input)
        : Input{input, reinterpret_cast<core::NativeWindow>(GetStdHandle(STD_OUTPUT_HANDLE)), systemCalls}
    {
    }

    Input::Input(core::NativeWindow input, const InputCalls &calls)
        : Input{input, reinterpret_cast<core::NativeWindow>(GetStdHandle(STD_OUTPUT_HANDLE)), calls}
    {
    }

    Input::Input(core::NativeWindow input, core::NativeWindow output, const InputCalls &calls)
        : input_{input}, output_{output}, calls_{calls}
    {
    }

    void Input::release(core::Button button)
    {
        calls_.mouseEvent(mouseFlag(button, false), 0, 0, 0, 0);
    }

    void Input::press(core::Button button)
    {
        calls_.mouseEvent(MOUSEEVENTF_MOVE | mouseFlag(button, true), 0, 0, 0, 0);
    }

    core::ReplayOutcome Input::replay(std::span<const core::MouseEvent> events)
    {
        const auto requested = static_cast<DWORD>(events.size());
        CONSOLE_SCREEN_BUFFER_INFO buffer{};
        const auto output = reinterpret_cast<HANDLE>(output_);
        if (calls_.getConsoleScreenBufferInfo(output, &buffer) == FALSE) {
            return {.status = FALSE, .requested = requested, .written = 0};
        }
        const auto offset = core::consoleRowOffset(buffer.dwSize.Y, buffer.srWindow.Top, buffer.srWindow.Bottom);
        if (!offset) {
            return {.status = FALSE, .requested = requested, .written = 0};
        }

        std::vector<INPUT_RECORD> records;
        records.reserve(events.size());
        for (const auto &event : events) {
            records.push_back(consoleRecord(event));
            records.back().Event.MouseEvent.dwMousePosition.Y =
                static_cast<SHORT>(records.back().Event.MouseEvent.dwMousePosition.Y + *offset);
        }
        DWORD written{};
        const auto input = reinterpret_cast<HANDLE>(input_);
        // Far's console::WriteInput adds the window-mode buffer delta before WriteConsoleInputW. In contrast,
        // records replaced through PluginManager::ProcessConsoleInput stay window-relative because keyboard.cpp
        // returns them directly to Far after plugin processing; only this replay boundary needs the conversion.
        const auto status = calls_.writeConsoleInput(input, records.data(), requested, &written);
        return {.status = status, .requested = requested, .written = written};
    }

    const InputCalls &systemInputCalls()
    {
        return systemCalls;
    }

} // namespace burlak::adapters::win
