#include "adapters/win/WindowProperties.hpp"

namespace burlak::adapters::win
{

    namespace
    {

        constexpr wchar_t receiverProperty[] = L"Burlak.FocusedReceiver.v1";
        const WindowPropertyCalls calls{SetPropW, GetPropW, RemovePropW, GetCurrentProcessId};

    } // namespace

    WindowProperties::WindowProperties() : calls_{calls}
    {
    }

    WindowProperties::WindowProperties(const WindowPropertyCalls &api) : calls_{api}
    {
    }

    void WindowProperties::set(core::NativeWindow window, std::uint32_t value)
    {
        static_cast<void>(calls_.set(reinterpret_cast<HWND>(window), receiverProperty,
                                     reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value))));
    }

    std::optional<std::uint32_t> WindowProperties::value(core::NativeWindow window) const
    {
        const auto stored =
            reinterpret_cast<std::uintptr_t>(calls_.get(reinterpret_cast<HWND>(window), receiverProperty));
        return stored == 0 ? std::nullopt : std::optional{static_cast<std::uint32_t>(stored)};
    }

    void WindowProperties::remove(core::NativeWindow window)
    {
        static_cast<void>(calls_.remove(reinterpret_cast<HWND>(window), receiverProperty));
    }

    std::uint32_t WindowProperties::processId() const
    {
        return calls_.getProcessId();
    }

    const WindowPropertyCalls &systemWindowPropertyCalls()
    {
        return calls;
    }

} // namespace burlak::adapters::win
