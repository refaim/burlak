#include "drag/DragSource.hpp"

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

            [[nodiscard]] core::DragAction query(core::Button value, bool escapePressed, bool leftDown,
                                                 bool rightDown) const override
            {
                button = value;
                escape = escapePressed;
                left = leftDown;
                right = rightDown;
                return action;
            }

            [[nodiscard]] core::Effect feedback(bool move, bool copy) const override
            {
                left = move;
                right = copy;
                return effect;
            }
        };

    } // namespace

    TEST_SUITE("drag source")
    {
        TEST_CASE("QueryInterface follows COM identity and reference counting")
        {
            Policy policy;
            DragSource source{policy, core::Button::Left};
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
            DragSource source{policy, core::Button::Right};
            policy.action = core::DragAction::Cancel;
            CHECK(source.QueryContinueDrag(TRUE, MK_LBUTTON | MK_RBUTTON) == DRAGDROP_S_CANCEL);
            CHECK(policy.button == core::Button::Right);
            CHECK(policy.escape);
            CHECK(policy.left);
            CHECK(policy.right);
            policy.action = core::DragAction::Drop;
            CHECK(source.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_DROP);
            policy.action = core::DragAction::Continue;
            CHECK(source.QueryContinueDrag(FALSE, MK_RBUTTON) == S_OK);
        }

        TEST_CASE("feedback records the latest effect and keeps the system cursors")
        {
            Policy policy;
            DragSource source{policy, core::Button::Left};
            policy.effect = core::Effect::Copy;
            CHECK(source.GiveFeedback(DROPEFFECT_COPY) == DRAGDROP_S_USEDEFAULTCURSORS);
            CHECK(source.lastEffect() == core::Effect::Copy);
            CHECK_FALSE(policy.left);
            CHECK(policy.right);
            policy.effect = core::Effect::Move;
            CHECK(source.GiveFeedback(DROPEFFECT_MOVE) == DRAGDROP_S_USEDEFAULTCURSORS);
            CHECK(source.lastEffect() == core::Effect::Move);
            policy.effect = core::Effect::None;
            CHECK(source.GiveFeedback(DROPEFFECT_NONE) == DRAGDROP_S_USEDEFAULTCURSORS);
            CHECK(source.lastEffect() == core::Effect::None);
        }
    }

} // namespace burlak::drag
