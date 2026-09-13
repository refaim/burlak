#pragma once

#include "core/Interfaces.hpp"

#include <optional>

namespace burlak::core
{

    enum class VerdictAction : std::uint8_t
    {
        Pass,
        Hold,
        Replace
    };

    struct Verdict
    {
        VerdictAction action{VerdictAction::Pass};
        std::optional<MouseEvent> replacement;
    };

    class Gesture
    {
      public:
        Gesture(IPanels &panels, IFarHost &host);

        [[nodiscard]] Verdict feed(const MouseEvent &event, bool dragActive = false);
        [[nodiscard]] std::optional<Button> synchro();
        void reset();

      private:
        enum class Phase : std::uint8_t
        {
            Idle,
            Armed,
            Starting,
            Spent
        };

        [[nodiscard]] Verdict arm(Button button, const MouseEvent &event);
        [[nodiscard]] bool onPanel(Cell cell);

        IPanels &panels_;
        IFarHost &host_;
        Phase phase_{Phase::Idle};
        Button button_{Button::Left};
        MouseEvent press_{};
    };

} // namespace burlak::core
