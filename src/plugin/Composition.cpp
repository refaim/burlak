#include "plugin/Composition.hpp"

#include "adapters/far/FarApi.hpp"
#include "adapters/shell/Shell.hpp"
#include "adapters/win/Files.hpp"
#include "adapters/win/Focus.hpp"
#include "adapters/win/Input.hpp"
#include "adapters/win/Peers.hpp"
#include "adapters/win/Screen.hpp"
#include "core/Session.hpp"
#include "drag/ExtractionWait.hpp"
#include "drag/ToolWindow.hpp"

#include <utility>

namespace burlak::plugin
{

    class Composition::Runtime
    {
      public:
        Runtime(const PluginStartupInfo &startupInfo,
                const std::optional<std::reference_wrapper<core::IScreen>> &screenOverride,
                const std::optional<std::reference_wrapper<core::IShell>> &shellOverride)
            : startupInfo_{startupInfo}, panels_{startupInfo_}, host_{startupInfo_},
              screen_{screenOverride ? screenOverride->get() : static_cast<core::IScreen &>(realScreen_)},
              shell_{shellOverride ? shellOverride->get() : static_cast<core::IShell &>(realShell_)},
              gesture_{panels_, host_}, session_{panels_, host_, screen_, input_, files_, shell_},
              extraction_{session_}, peers_{focus_}, tool_{screen_, input_, shell_, session_, extraction_, peers_}
        {
            files_.sweep();
            static_cast<void>(tool_.start());
        }

        [[nodiscard]] core::Verdict feed(const core::MouseEvent &event)
        {
            return gesture_.feed(event, tool_.active());
        }

        void synchro()
        {
            // The Far thread owns this scope guard. It signals a pending tool-thread wait with failure if any
            // permitted exception reaches the export firewall before Session can publish an outcome.
            drag::ExtractionCompleter completion{extraction_};
            const auto start = gesture_.synchro();
            if (start) {
                static_cast<void>(session_.begin(tool_, *start));
            }
            completion.complete(session_.synchro());
        }

        void recordFocus()
        {
            focus_.record();
        }

        [[nodiscard]] std::uint64_t lastFocus() const
        {
            return focus_.last();
        }

        [[nodiscard]] core::NativeWindow toolWindow() const
        {
            return tool_.nativeWindow();
        }

        void stop()
        {
            extraction_.cancel();
            tool_.stop();
            files_.sweep();
        }

      private:
        PluginStartupInfo startupInfo_{};
        adapters::far_api::FarPanels panels_;
        adapters::far_api::FarHost host_;
        adapters::win::Files files_;
        adapters::win::Screen realScreen_;
        adapters::win::Input input_;
        adapters::shell::Shell realShell_;
        adapters::win::Focus focus_;
        core::IScreen &screen_;
        core::IShell &shell_;
        core::Gesture gesture_;
        core::Session session_;
        drag::ExtractionWait extraction_;
        adapters::win::Peers peers_;
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
        runtime_ = std::make_unique<Runtime>(info, screenOverride_, shellOverride_);
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

    void Composition::recordFocus()
    {
        if (runtime_) {
            runtime_->recordFocus();
        }
    }

    std::uint64_t Composition::lastFocus() const
    {
        return runtime_ ? runtime_->lastFocus() : 0;
    }

    core::NativeWindow Composition::toolWindow() const
    {
        return runtime_ ? runtime_->toolWindow() : 0;
    }

    void Composition::usePeerDropAdapters(core::IScreen &screen, core::IShell &shell)
    {
        reset();
        screenOverride_ = screen;
        shellOverride_ = shell;
    }

    void Composition::useDefaultAdapters()
    {
        reset();
        screenOverride_.reset();
        shellOverride_.reset();
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
