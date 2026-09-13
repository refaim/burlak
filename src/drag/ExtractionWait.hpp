#pragma once

#include "core/Interfaces.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
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
        void cancel() noexcept;
        void complete(bool succeeded) noexcept;
        void complete(std::optional<bool> succeeded) noexcept;

      private:
        enum class State : std::uint8_t
        {
            Idle,
            Waiting,
            Cancelled,
        };

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
        std::atomic<State> state_{State::Idle};
    };

    class ExtractionCompleter final
    {
      public:
        explicit ExtractionCompleter(ExtractionWait &wait) noexcept;
        ~ExtractionCompleter();

        void complete(std::optional<bool> succeeded) noexcept;

      private:
        ExtractionCompleter(const ExtractionCompleter &);
        ExtractionCompleter &operator=(const ExtractionCompleter &);

        ExtractionWait &wait_;
        bool completed_{};
    };

    [[nodiscard]] const ExtractionWaitCalls &systemExtractionWaitCalls();

} // namespace burlak::drag
