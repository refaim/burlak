#pragma once

#include "core/Interfaces.hpp"

#include <windows.h>

namespace burlak::adapters::win
{

    struct InputCalls
    {
        void(WINAPI *mouseEvent)(DWORD flags, DWORD x, DWORD y, DWORD data, ULONG_PTR extraInfo);
        decltype(&WriteConsoleInputW) writeConsoleInput;
    };

    class Input final : public core::IInput
    {
      public:
        Input();
        explicit Input(core::NativeWindow input);
        Input(core::NativeWindow input, const InputCalls &calls);

        void release(core::Button button) override;
        void press(core::Button button) override;
        [[nodiscard]] core::ReplayOutcome replay(std::span<const core::MouseEvent> events) override;

      private:
        core::NativeWindow input_{};
        const InputCalls &calls_;
    };

    [[nodiscard]] const InputCalls &systemInputCalls();

} // namespace burlak::adapters::win
