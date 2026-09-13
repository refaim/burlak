#pragma once

#include "core/Interfaces.hpp"

#include <windows.h>

#include <atomic>
#include <memory>

namespace burlak::drag
{

    struct ExtractionWaitCalls
    {
        decltype(&CreateEventW) createEvent;
        decltype(&ResetEvent) resetEvent;
        decltype(&SetEvent) setEvent;
        decltype(&CloseHandle) closeHandle;
        decltype(&CoWaitForMultipleHandles) coWait;
    };

    class ExtractionWait final : public core::IExtraction
    {
      public:
        explicit ExtractionWait(core::IExtractionSession &session);
        ExtractionWait(core::IExtractionSession &session, const ExtractionWaitCalls &calls);

        [[nodiscard]] bool extract() override;
        void cleanup() override;
        void complete(bool succeeded);
        void complete(std::optional<bool> succeeded);

      private:
        struct EventHandle
        {
            decltype(&CloseHandle) close;
            void operator()(void *handle) const noexcept;
        };

        using UniqueEvent = std::unique_ptr<void, EventHandle>;

        core::IExtractionSession &session_;
        const ExtractionWaitCalls &calls_;
        UniqueEvent event_;
        std::atomic<bool> succeeded_{};
    };

    [[nodiscard]] const ExtractionWaitCalls &systemExtractionWaitCalls();

} // namespace burlak::drag
