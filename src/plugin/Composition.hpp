#pragma once

#include "core/Gesture.hpp"
#include "core/Interfaces.hpp"

#include <plugin.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace burlak::plugin
{

    class Composition
    {
      public:
        Composition();
        ~Composition();

        void setStartupInfo(const PluginStartupInfo &info);
        void reset();
        [[nodiscard]] core::Verdict feed(const core::MouseEvent &event);
        void recordFocus();
        [[nodiscard]] std::uint64_t lastFocus() const;
        [[nodiscard]] core::NativeWindow toolWindow() const;
        void usePeerDropAdapters(core::IScreen &screen, core::IShell &shell);
        void usePeerDropAdapters(core::IScreen &screen, core::IShell &shell, core::IPeers &peers);
        void useDefaultAdapters();
        void synchro();
        void stop();

      private:
        class Runtime;
        std::unique_ptr<Runtime> runtime_;
        std::optional<std::reference_wrapper<core::IScreen>> screenOverride_;
        std::optional<std::reference_wrapper<core::IShell>> shellOverride_;
        std::optional<std::reference_wrapper<core::IPeers>> peersOverride_;
    };

    [[nodiscard]] Composition &composition();

} // namespace burlak::plugin
