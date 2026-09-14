#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>

namespace burlak::adapters::win
{

    struct FocusCalls
    {
        decltype(&GetTickCount64) tickCount;
    };

    class Focus final
    {
      public:
        Focus();
        explicit Focus(const FocusCalls &calls);

        void record();
        [[nodiscard]] std::uint64_t last() const;

      private:
        const FocusCalls &calls_;
        std::atomic<std::uint64_t> last_{};
    };

    [[nodiscard]] const FocusCalls &systemFocusCalls();

} // namespace burlak::adapters::win
