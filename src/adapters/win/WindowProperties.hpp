#pragma once

#include "core/Interfaces.hpp"

#include <windows.h>

namespace burlak::adapters::win
{

    struct WindowPropertyCalls
    {
        decltype(&SetPropW) set;
        decltype(&GetPropW) get;
        decltype(&RemovePropW) remove;
        decltype(&GetCurrentProcessId) getProcessId;
    };

    class WindowProperties final : public core::IWindowProperties
    {
      public:
        WindowProperties();
        explicit WindowProperties(const WindowPropertyCalls &calls);

        void set(core::NativeWindow window, std::uint32_t value) override;
        [[nodiscard]] std::optional<std::uint32_t> value(core::NativeWindow window) const override;
        void remove(core::NativeWindow window) override;
        [[nodiscard]] std::uint32_t processId() const override;

      private:
        const WindowPropertyCalls &calls_;
    };

    [[nodiscard]] const WindowPropertyCalls &systemWindowPropertyCalls();

} // namespace burlak::adapters::win
