#include "adapters/win/Input.hpp"

#include <windows.h>

#include <vector>

namespace burlak::adapters::win
{

    namespace
    {

        const InputCalls systemCalls{mouse_event, WriteConsoleInputW};

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

    Input::Input() : Input{reinterpret_cast<core::NativeWindow>(GetStdHandle(STD_INPUT_HANDLE)), systemCalls}
    {
    }

    Input::Input(core::NativeWindow input) : Input{input, systemCalls}
    {
    }

    Input::Input(core::NativeWindow input, const InputCalls &calls) : input_{input}, calls_{calls}
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
        std::vector<INPUT_RECORD> records;
        records.reserve(events.size());
        for (const auto &event : events) {
            records.push_back(consoleRecord(event));
        }
        DWORD written{};
        const auto input = reinterpret_cast<HANDLE>(input_);
        const auto requested = static_cast<DWORD>(records.size());
        const auto status = calls_.writeConsoleInput(input, records.data(), requested, &written);
        return {.status = status, .requested = requested, .written = written};
    }

    const InputCalls &systemInputCalls()
    {
        return systemCalls;
    }

} // namespace burlak::adapters::win
