#pragma once

namespace burlak::plugin
{

    template <class Action> void firewall(Action &&action) noexcept
    {
        try {
            action();
        } catch (...) {
            // Far's C ABI cannot accept an exception from any of the seven exported entry points.
            return;
        }
    }

} // namespace burlak::plugin
