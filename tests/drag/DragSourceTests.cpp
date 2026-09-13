#include "drag/DragSource.hpp"
#include "drag/ExtractionWait.hpp"

#include <doctest/doctest.h>

namespace burlak::drag
{

    namespace
    {

        class Policy final : public core::IReleasePolicy
        {
          public:
            core::DragAction action{core::DragAction::Continue};
            core::Effect effect{core::Effect::None};
            mutable core::Button button{};
            mutable bool escape{};
            mutable bool left{};
            mutable bool right{};
            mutable core::Effect lastEffect{};
            mutable bool needsExtraction{};
            mutable bool overOwnWindow{};

            [[nodiscard]] core::DragAction query(core::Button value, bool escapePressed, bool leftDown, bool rightDown,
                                                 core::Effect feedback, bool extraction, bool ownWindow) const override
            {
                button = value;
                escape = escapePressed;
                left = leftDown;
                right = rightDown;
                lastEffect = feedback;
                needsExtraction = extraction;
                overOwnWindow = ownWindow;
                return action;
            }

            [[nodiscard]] core::Effect feedback(bool move, bool copy, bool link) const override
            {
                left = move;
                right = copy;
                escape = link;
                return effect;
            }
        };

        class Screen final : public core::IScreen
        {
          public:
            std::optional<core::Point> point{core::Point{10, 20}};
            core::NativeWindow window{9};
            int cursorCalls{};

            [[nodiscard]] std::optional<core::Point> cursor() override
            {
                ++cursorCalls;
                return point;
            }

            [[nodiscard]] bool buttonDown(core::Button) override
            {
                return false;
            }

            [[nodiscard]] core::NativeWindow windowAt(core::Point) override
            {
                return window;
            }

            [[nodiscard]] std::optional<core::HostWindow> hostWindow() override
            {
                return std::nullopt;
            }

            [[nodiscard]] std::optional<core::HostWindow> hostWindowAt(core::Point) override
            {
                return std::nullopt;
            }

            [[nodiscard]] std::expected<core::CellGeometry, core::Error> cellGeometry() override
            {
                return std::unexpected(core::Error::Unavailable);
            }

            [[nodiscard]] std::expected<core::CellGeometry, core::Error> cellGeometryAt(core::Point) override
            {
                return std::unexpected(core::Error::Unavailable);
            }
        };

        class Extraction final : public core::IExtraction
        {
          public:
            bool succeeds{true};
            int calls{};
            int cleanups{};

            [[nodiscard]] bool extract() override
            {
                ++calls;
                return succeeds;
            }

            void cleanup() override
            {
                ++cleanups;
            }
        };

        class ExtractionHost final : public core::IExtractionSession
        {
          public:
            int requests{};

            [[nodiscard]] bool requestExtraction() override
            {
                ++requests;
                return true;
            }

            void cleanup() override
            {
            }
        };

        ExtractionWait *activeWait{};
        bool waitOutcome{};
        bool scopedOutcome{};

        HANDLE WINAPI createEvent(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCWSTR)
        {
            return CreateEventW(nullptr, TRUE, FALSE, nullptr);
        }

        HRESULT WINAPI waitForExtraction(DWORD, DWORD, ULONG, LPHANDLE, LPDWORD index)
        {
            *index = 0;
            activeWait->complete(waitOutcome);
            return S_OK;
        }

        HRESULT WINAPI waitForExceptionalCompletion(DWORD, DWORD, ULONG, LPHANDLE handles, LPDWORD index)
        {
            bool caught{};
            try {
                ExtractionCompleter completion{*activeWait};
                throw 1;
            } catch (...) {
                caught = true;
            }
            if (!caught) {
                return E_FAIL;
            }
            *index = 0;
            return WaitForSingleObject(handles[0], 0) == WAIT_OBJECT_0 ? S_OK : E_FAIL;
        }

        HRESULT WINAPI waitForScopedCompletion(DWORD, DWORD, ULONG, LPHANDLE handles, LPDWORD index)
        {
            {
                ExtractionCompleter completion{*activeWait};
                completion.complete(std::optional{scopedOutcome});
            }
            *index = 0;
            return WaitForSingleObject(handles[0], 0) == WAIT_OBJECT_0 ? S_OK : E_FAIL;
        }

    } // namespace

    TEST_SUITE("drag source")
    {
        TEST_CASE("QueryInterface follows COM identity and reference counting")
        {
            Policy policy;
            Screen screen;
            Extraction extraction;
            DragSource source{policy, screen, extraction, core::Button::Left, 7, false};
            CHECK(source.QueryInterface(IID_IUnknown, nullptr) == E_POINTER);

            void *object{};
            CHECK(source.QueryInterface(IID_IUnknown, &object) == S_OK);
            CHECK(object == static_cast<IDropSource *>(&source));
            CHECK(source.Release() == 1);

            object = nullptr;
            CHECK(source.QueryInterface(IID_IDropSource, &object) == S_OK);
            CHECK(object == static_cast<IDropSource *>(&source));
            CHECK(source.Release() == 1);

            const GUID unknown{0x12345678, 0, 0, {}};
            CHECK(source.QueryInterface(unknown, &object) == E_NOINTERFACE);
            CHECK(object == nullptr);
            CHECK(source.AddRef() == 2);
            CHECK(source.Release() == 1);
        }

        TEST_CASE("QueryContinueDrag marshals the button state to the core policy")
        {
            Policy policy;
            Screen screen;
            Extraction extraction;
            DragSource source{policy, screen, extraction, core::Button::Right, 7, true};
            policy.action = core::DragAction::Cancel;
            CHECK(source.QueryContinueDrag(TRUE, MK_LBUTTON | MK_RBUTTON) == DRAGDROP_S_CANCEL);
            CHECK(policy.button == core::Button::Right);
            CHECK(policy.escape);
            CHECK(policy.left);
            CHECK(policy.right);
            CHECK(policy.needsExtraction);
            CHECK_FALSE(policy.overOwnWindow);
            policy.action = core::DragAction::Drop;
            CHECK(source.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_DROP);
            policy.action = core::DragAction::Continue;
            CHECK(source.QueryContinueDrag(FALSE, MK_RBUTTON) == S_OK);
        }

        TEST_CASE("feedback records the latest effect and keeps the system cursors")
        {
            Policy policy;
            Screen screen;
            Extraction extraction;
            DragSource source{policy, screen, extraction, core::Button::Left, 7, false};
            policy.effect = core::Effect::Copy;
            CHECK(source.GiveFeedback(DROPEFFECT_COPY) == DRAGDROP_S_USEDEFAULTCURSORS);
            CHECK(source.lastEffect() == core::Effect::Copy);
            CHECK_FALSE(policy.left);
            CHECK(policy.right);
            CHECK_FALSE(policy.escape);
            policy.effect = core::Effect::Move;
            CHECK(source.GiveFeedback(DROPEFFECT_MOVE) == DRAGDROP_S_USEDEFAULTCURSORS);
            CHECK(source.lastEffect() == core::Effect::Move);
            policy.effect = core::Effect::None;
            CHECK(source.GiveFeedback(DROPEFFECT_NONE) == DRAGDROP_S_USEDEFAULTCURSORS);
            CHECK(source.lastEffect() == core::Effect::None);
            policy.effect = core::Effect::Link;
            CHECK(source.GiveFeedback(DROPEFFECT_LINK) == DRAGDROP_S_USEDEFAULTCURSORS);
            CHECK(source.lastEffect() == core::Effect::Link);
            CHECK(policy.escape);
        }

        TEST_CASE("placeholder-backed feedback never accepts a link effect")
        {
            Policy policy;
            Screen screen;
            Extraction extraction;
            DragSource source{policy, screen, extraction, core::Button::Left, 7, true};
            policy.effect = core::Effect::Link;

            CHECK(source.GiveFeedback(DROPEFFECT_LINK) == DRAGDROP_S_USEDEFAULTCURSORS);
            CHECK_FALSE(policy.escape);
        }

        TEST_CASE("an external plugin-panel release waits for extraction and maps its outcome")
        {
            Policy policy;
            policy.action = core::DragAction::ExtractThenDrop;
            policy.effect = core::Effect::Copy;
            Screen screen;
            ExtractionHost host;
            const ExtractionWaitCalls calls{createEvent, ResetEvent, SetEvent, CloseHandle, waitForExtraction};
            ExtractionWait extraction{host, calls};
            activeWait = &extraction;
            DragSource source{policy, screen, extraction, core::Button::Left, 7, true};
            REQUIRE(source.GiveFeedback(DROPEFFECT_COPY) == DRAGDROP_S_USEDEFAULTCURSORS);

            waitOutcome = true;
            CHECK(source.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_DROP);
            CHECK(host.requests == 1);

            waitOutcome = false;
            CHECK(source.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_CANCEL);
            CHECK(host.requests == 2);
        }

        TEST_CASE("an exceptional Far-thread exit completes the extraction wait with failure")
        {
            ExtractionHost host;
            const ExtractionWaitCalls calls{createEvent, ResetEvent, SetEvent, CloseHandle,
                                            waitForExceptionalCompletion};
            ExtractionWait extraction{host, calls};
            activeWait = &extraction;

            CHECK_FALSE(extraction.extract());
            CHECK(host.requests == 1);
        }

        TEST_CASE("a completed Far-thread scope signals exactly the stored extraction outcome")
        {
            ExtractionHost host;
            const ExtractionWaitCalls calls{createEvent, ResetEvent, SetEvent, CloseHandle, waitForScopedCompletion};
            ExtractionWait extraction{host, calls};
            activeWait = &extraction;

            scopedOutcome = true;
            CHECK(extraction.extract());
            scopedOutcome = false;
            CHECK_FALSE(extraction.extract());
            CHECK(host.requests == 2);
        }

        TEST_CASE("the release policy sees whether the cursor is over Burlak's own window")
        {
            Policy policy;
            policy.action = core::DragAction::Drop;
            policy.effect = core::Effect::Move;
            Screen screen;
            screen.window = 7;
            Extraction extraction;
            DragSource source{policy, screen, extraction, core::Button::Left, 7, true};
            REQUIRE(source.GiveFeedback(DROPEFFECT_MOVE) == DRAGDROP_S_USEDEFAULTCURSORS);

            CHECK(source.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_DROP);
            CHECK(policy.overOwnWindow);
            CHECK(policy.lastEffect == core::Effect::Move);
            CHECK(extraction.calls == 0);

            screen.point.reset();
            CHECK(source.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_DROP);
            CHECK_FALSE(policy.overOwnWindow);
        }

        TEST_CASE("a real-path release never queries the window beneath the cursor")
        {
            Policy policy;
            policy.action = core::DragAction::Drop;
            policy.effect = core::Effect::Move;
            Screen screen;
            Extraction extraction;
            DragSource source{policy, screen, extraction, core::Button::Left, 7, false};
            REQUIRE(source.GiveFeedback(DROPEFFECT_MOVE) == DRAGDROP_S_USEDEFAULTCURSORS);

            CHECK(source.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_DROP);
            CHECK_FALSE(policy.needsExtraction);
            CHECK(screen.cursorCalls == 0);
        }
    }

} // namespace burlak::drag
