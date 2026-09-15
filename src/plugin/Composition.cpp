#include "plugin/Composition.hpp"

#include "adapters/far/FarApi.hpp"
#include "adapters/shell/Shell.hpp"
#include "adapters/win/DropMenu.hpp"
#include "adapters/win/Files.hpp"
#include "adapters/win/Input.hpp"
#include "adapters/win/Screen.hpp"
#include "adapters/win/WindowProperties.hpp"
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
                const std::optional<std::reference_wrapper<core::IShell>> &shellOverride,
                const std::optional<std::reference_wrapper<core::IFiles>> &filesOverride,
                const std::optional<std::reference_wrapper<core::IWindowProperties>> &propertiesOverride,
                const std::optional<std::reference_wrapper<core::IDropMenu>> &menuOverride,
                const std::optional<std::reference_wrapper<core::IInput>> &inputOverride)
            : startupInfo_{startupInfo}, panels_{startupInfo_}, host_{startupInfo_},
              screen_{screenOverride ? screenOverride->get() : static_cast<core::IScreen &>(realScreen_)},
              input_{inputOverride ? inputOverride->get() : static_cast<core::IInput &>(realInput_)},
              shell_{shellOverride ? shellOverride->get() : static_cast<core::IShell &>(realShell_)},
              files_{filesOverride ? filesOverride->get() : static_cast<core::IFiles &>(realFiles_)},
              properties_{propertiesOverride ? propertiesOverride->get()
                                             : static_cast<core::IWindowProperties &>(realProperties_)},
              menu_{menuOverride ? menuOverride->get() : static_cast<core::IDropMenu &>(realMenu_)},
              gesture_{panels_, host_}, session_{panels_, host_, screen_, input_, files_, shell_},
              extraction_{session_},
              tool_{screen_, input_, shell_, realDropData_, session_, extraction_, files_, properties_, menu_}
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
            if (auto snapshot = session_.takeReceiveSnapshot()) {
                tool_.receiveSnapshot(std::move(*snapshot));
            }
            if (const auto refresh = session_.takeReceiveRefresh()) {
                tool_.completeReceiveRefresh(*refresh);
            }
        }

        void recordFocus()
        {
            const auto host = screen_.hostWindowHandle();
            if (host != 0) {
                properties_.set(host, properties_.processId());
            }
        }

        [[nodiscard]] core::NativeWindow toolWindow() const
        {
            return tool_.nativeWindow();
        }

        void dropOnToolWindow(core::Point point, bool shift)
        {
            tool_.drop(point, shift);
        }

        [[nodiscard]] std::uint32_t dragEnterToolWindow(std::uintptr_t dataObject, std::uint32_t keyState,
                                                        core::Point point, std::uint32_t allowedEffects)
        {
            return tool_.dragEnter(dataObject, keyState, point, allowedEffects);
        }

        [[nodiscard]] std::uint32_t dropOnToolWindow(std::uintptr_t dataObject, std::uint32_t keyState,
                                                     core::Point point, std::uint32_t allowedEffects)
        {
            return tool_.drop(dataObject, keyState, point, allowedEffects);
        }

        void stop()
        {
            if (std::exchange(stopped_, true)) {
                return;
            }
            const auto host = screen_.hostWindowHandle();
            if (host != 0 && properties_.value(host) == properties_.processId()) {
                properties_.remove(host);
            }
            extraction_.cancel();
            tool_.completeReceiveRefresh(false);
            tool_.stop();
            files_.sweep();
        }

      private:
        PluginStartupInfo startupInfo_{};
        adapters::far_api::FarPanels panels_;
        adapters::far_api::FarHost host_;
        adapters::win::Files realFiles_;
        adapters::win::Screen realScreen_;
        adapters::win::Input realInput_;
        adapters::shell::Shell realShell_;
        adapters::shell::DropData realDropData_;
        adapters::win::WindowProperties realProperties_;
        adapters::win::DropMenu realMenu_;
        core::IScreen &screen_;
        core::IInput &input_;
        core::IShell &shell_;
        core::IFiles &files_;
        core::IWindowProperties &properties_;
        core::IDropMenu &menu_;
        core::Gesture gesture_;
        core::Session session_;
        drag::ExtractionWait extraction_;
        drag::ToolWindow tool_;
        bool stopped_{};
    };

    Composition::Composition() = default;

    Composition::~Composition()
    {
        stop();
    }

    void Composition::setStartupInfo(const PluginStartupInfo &info)
    {
        reset();
        runtime_ = std::make_unique<Runtime>(info, screenOverride_, shellOverride_, filesOverride_, propertiesOverride_,
                                             menuOverride_, inputOverride_);
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

    core::NativeWindow Composition::toolWindow() const
    {
        return runtime_ ? runtime_->toolWindow() : 0;
    }

    void Composition::dropOnToolWindow(core::Point point, bool shift)
    {
        if (runtime_) {
            runtime_->dropOnToolWindow(point, shift);
        }
    }

    std::uint32_t Composition::dragEnterToolWindow(std::uintptr_t dataObject, std::uint32_t keyState, core::Point point,
                                                   std::uint32_t allowedEffects)
    {
        return runtime_ ? runtime_->dragEnterToolWindow(dataObject, keyState, point, allowedEffects) : 0;
    }

    std::uint32_t Composition::dropOnToolWindow(std::uintptr_t dataObject, std::uint32_t keyState, core::Point point,
                                                std::uint32_t allowedEffects)
    {
        return runtime_ ? runtime_->dropOnToolWindow(dataObject, keyState, point, allowedEffects) : 0;
    }

    void Composition::useReceiveAdapters(core::IScreen &screen, core::IShell &shell, core::IFiles &files,
                                         core::IWindowProperties &properties, core::IDropMenu &menu)
    {
        reset();
        screenOverride_ = screen;
        shellOverride_ = shell;
        filesOverride_ = files;
        propertiesOverride_ = properties;
        menuOverride_ = menu;
    }

    void Composition::useInputAdapter(core::IInput &input)
    {
        reset();
        inputOverride_ = input;
    }

    void Composition::useDefaultAdapters()
    {
        reset();
        screenOverride_.reset();
        shellOverride_.reset();
        filesOverride_.reset();
        propertiesOverride_.reset();
        menuOverride_.reset();
        inputOverride_.reset();
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
