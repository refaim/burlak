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
        [[nodiscard]] core::NativeWindow toolWindow() const;
        void dropOnToolWindow(core::Point point, bool shift);
        [[nodiscard]] std::uint32_t dragEnterToolWindow(std::uintptr_t dataObject, std::uint32_t keyState,
                                                        core::Point point, std::uint32_t allowedEffects);
        [[nodiscard]] std::uint32_t dropOnToolWindow(std::uintptr_t dataObject, std::uint32_t keyState,
                                                     core::Point point, std::uint32_t allowedEffects);
        void useReceiveAdapters(core::IScreen &screen, core::IShell &shell, core::IFiles &files,
                                core::IWindowProperties &properties, core::IDropMenu &menu);
        void useInputAdapter(core::IInput &input);
        void useDefaultAdapters();
        void synchro();
        void stop();

      private:
        class Runtime;
        std::unique_ptr<Runtime> runtime_;
        std::optional<std::reference_wrapper<core::IScreen>> screenOverride_;
        std::optional<std::reference_wrapper<core::IShell>> shellOverride_;
        std::optional<std::reference_wrapper<core::IFiles>> filesOverride_;
        std::optional<std::reference_wrapper<core::IWindowProperties>> propertiesOverride_;
        std::optional<std::reference_wrapper<core::IDropMenu>> menuOverride_;
        std::optional<std::reference_wrapper<core::IInput>> inputOverride_;
    };

    [[nodiscard]] Composition &composition();

} // namespace burlak::plugin
