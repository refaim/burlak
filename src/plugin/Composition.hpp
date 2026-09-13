#pragma once

#include "core/Gesture.hpp"

#include <plugin.hpp>

#include <memory>

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
        void synchro();
        void stop();

      private:
        class Runtime;
        std::unique_ptr<Runtime> runtime_;
    };

    [[nodiscard]] Composition &composition();

} // namespace burlak::plugin
