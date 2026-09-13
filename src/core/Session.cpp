#include "core/Session.hpp"

#include "core/DragPlan.hpp"

namespace burlak::core
{

    Session::Session(IPanels &panels, IScreen &screen, IInput &input, IDragTool &tool)
        : panels_{panels}, screen_{screen}, input_{input}, tool_{tool}
    {
    }

    bool Session::begin(Button button)
    {
        const auto paths = DragPlan{panels_}.paths();
        if (!paths || !tool_.start()) {
            return false;
        }

        if (!screen_.buttonDown(button)) {
            return false;
        }
        // Namespace lookup is slow, so 1.2.0 checked the physical button before paying that cost and
        // again immediately before synthesizing the release.
        if (!tool_.prepare(*paths, button)) {
            return false;
        }
        if (!screen_.hostWindow()) {
            tool_.abort();
            return false;
        }
        if (!screen_.buttonDown(button)) {
            tool_.abort();
            return false;
        }

        // The fresh press over the tool window must follow this release in the serial input stream.
        input_.release(button);
        return tool_.showAndArm();
    }

} // namespace burlak::core
