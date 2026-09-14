#include "adapters/win/Focus.hpp"

namespace burlak::adapters::win
{

    namespace
    {

        const FocusCalls calls{GetTickCount64};

    } // namespace

    Focus::Focus() : calls_{calls}
    {
    }

    Focus::Focus(const FocusCalls &api) : calls_{api}
    {
    }

    void Focus::record()
    {
        last_.store(calls_.tickCount());
    }

    std::uint64_t Focus::last() const
    {
        return last_.load();
    }

    const FocusCalls &systemFocusCalls()
    {
        return calls;
    }

} // namespace burlak::adapters::win
