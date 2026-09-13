#include "drag/ExtractionWait.hpp"

#include <array>

namespace burlak::drag
{

    namespace
    {

        const ExtractionWaitCalls systemCalls{CreateEventW, ResetEvent, SetEvent, CloseHandle,
                                              CoWaitForMultipleHandles};

    } // namespace

    void ExtractionWait::EventHandle::operator()(void *handle) const noexcept
    {
        static_cast<void>(close(handle));
    }

    ExtractionWait::ExtractionWait(core::IExtractionSession &session) : ExtractionWait{session, systemCalls}
    {
    }

    ExtractionWait::ExtractionWait(core::IExtractionSession &session, const ExtractionWaitCalls &calls)
        : session_{session}, calls_{calls},
          event_{calls_.createEvent(nullptr, TRUE, FALSE, nullptr), EventHandle{calls_.closeHandle}}
    {
    }

    bool ExtractionWait::extract()
    {
        if (!event_ || calls_.resetEvent(event_.get()) == FALSE) {
            return false;
        }
        succeeded_.store(false);
        auto idle = State::Idle;
        if (!state_.compare_exchange_strong(idle, State::Waiting)) {
            return false;
        }
        if (!session_.requestExtraction()) {
            auto waiting = State::Waiting;
            static_cast<void>(state_.compare_exchange_strong(waiting, State::Idle));
            return false;
        }

        std::array handles{static_cast<HANDLE>(event_.get())};
        DWORD signalled{};
        // Archive extraction has no timeout: the owner plugin may legitimately show progress for a large archive.
        constexpr DWORD pumpFlags =
            static_cast<DWORD>(COWAIT_DISPATCH_CALLS) | static_cast<DWORD>(COWAIT_DISPATCH_WINDOW_MESSAGES);
        const HRESULT status =
            calls_.coWait(pumpFlags, INFINITE, static_cast<ULONG>(handles.size()), handles.data(), &signalled);
        auto waiting = State::Waiting;
        static_cast<void>(state_.compare_exchange_strong(waiting, State::Idle));
        return status == S_OK && signalled == 0 && succeeded_.load();
    }

    void ExtractionWait::cleanup()
    {
        session_.cleanup();
    }

    void ExtractionWait::cancel() noexcept
    {
        if (state_.exchange(State::Cancelled) != State::Waiting) {
            return;
        }
        succeeded_.store(false);
        // Teardown owns this signal and sends it before joining the tool thread, closing the race where the
        // unbounded wait has posted synchro work that Far will no longer dispatch.
        static_cast<void>(calls_.setEvent(event_.get()));
    }

    void ExtractionWait::complete(bool succeeded) noexcept
    {
        auto waiting = State::Waiting;
        if (!state_.compare_exchange_strong(waiting, State::Idle)) {
            return;
        }
        succeeded_.store(succeeded);
        // Far's synchro thread owns this signal and sends it after publishing the extraction outcome.
        static_cast<void>(calls_.setEvent(event_.get()));
    }

    void ExtractionWait::complete(std::optional<bool> succeeded) noexcept
    {
        if (succeeded) {
            complete(*succeeded);
        }
    }

    ExtractionCompleter::ExtractionCompleter(ExtractionWait &wait) noexcept : wait_{wait}
    {
    }

    ExtractionCompleter::~ExtractionCompleter()
    {
        if (!completed_) {
            wait_.complete(false);
        }
    }

    void ExtractionCompleter::complete(std::optional<bool> succeeded) noexcept
    {
        if (succeeded) {
            wait_.complete(*succeeded);
            completed_ = true;
        }
    }

    const ExtractionWaitCalls &systemExtractionWaitCalls()
    {
        return systemCalls;
    }

} // namespace burlak::drag
