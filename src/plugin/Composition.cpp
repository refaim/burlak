#include "plugin/Composition.hpp"

#include "adapters/far/FarApi.hpp"
#include "adapters/shell/Shell.hpp"
#include "adapters/win/Input.hpp"
#include "adapters/win/Screen.hpp"
#include "core/Session.hpp"
#include "drag/ToolWindow.hpp"

#include <utility>

namespace burlak::plugin
{

    class Composition::Runtime
    {
      public:
        explicit Runtime(const PluginStartupInfo &startupInfo)
            : startupInfo_{startupInfo}, panels_{startupInfo_}, host_{startupInfo_}, gesture_{panels_, host_},
              session_{panels_, host_, screen_, input_}, tool_{screen_, input_, shell_, session_}
        {
        }

        [[nodiscard]] core::Verdict feed(const core::MouseEvent &event)
        {
            return gesture_.feed(event, tool_.active());
        }

        void synchro()
        {
            const auto start = gesture_.synchro();
            if (start) {
                static_cast<void>(session_.begin(tool_, *start));
            }
            session_.synchro();
        }

        void stop()
        {
            tool_.stop();
        }

      private:
        PluginStartupInfo startupInfo_{};
        adapters::far_api::FarPanels panels_;
        adapters::far_api::FarHost host_;
        adapters::win::Screen screen_;
        adapters::win::Input input_;
        adapters::shell::Shell shell_;
        core::Gesture gesture_;
        core::Session session_;
        drag::ToolWindow tool_;
    };

    Composition::Composition() = default;

    Composition::~Composition()
    {
        stop();
    }

    void Composition::setStartupInfo(const PluginStartupInfo &info)
    {
        reset();
        runtime_ = std::make_unique<Runtime>(info);
    }

    void Composition::reset()
    {
        stop();
        runtime_.reset();
    }

    core::Verdict Composition::feed(const core::MouseEvent &event)
    {
        return runtime_ ? runtime_->feed(event) : core::Verdict{};
    }

    void Composition::synchro()
    {
        if (runtime_) {
            runtime_->synchro();
        }
    }

    void Composition::stop()
    {
        if (runtime_) {
            runtime_->stop();
        }
    }

    Composition &composition()
    {
        static Composition instance;
        return instance;
    }

} // namespace burlak::plugin
