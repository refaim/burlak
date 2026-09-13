#include "drag/DropTarget.hpp"

#include <doctest/doctest.h>

namespace burlak::drag
{

    namespace
    {

        class Policy final : public core::IDropPolicy
        {
          public:
            core::Effect chosen{core::Effect::None};
            mutable bool move{};
            mutable bool copy{};

            [[nodiscard]] core::Effect effect(bool moveOffered, bool copyOffered) const override
            {
                move = moveOffered;
                copy = copyOffered;
                return chosen;
            }
        };

    } // namespace

    TEST_SUITE("drop target")
    {
        TEST_CASE("COM identity and the inert 1.2.0 target reject every effect")
        {
            Policy policy;
            DropTarget target{policy};
            CHECK(target.QueryInterface(IID_IUnknown, nullptr) == E_POINTER);

            void *object{};
            CHECK(target.QueryInterface(IID_IUnknown, &object) == S_OK);
            CHECK(object == static_cast<IDropTarget *>(&target));
            CHECK(target.Release() == 1);
            object = nullptr;
            CHECK(target.QueryInterface(IID_IDropTarget, &object) == S_OK);
            CHECK(target.Release() == 1);
            const GUID unknown{0x87654321, 0, 0, {}};
            CHECK(target.QueryInterface(unknown, &object) == E_NOINTERFACE);
            CHECK(object == nullptr);
            CHECK(target.AddRef() == 2);
            CHECK(target.Release() == 1);

            POINTL point{};
            DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
            CHECK(target.DragEnter(nullptr, 0, point, &effect) == S_OK);
            CHECK(effect == DROPEFFECT_NONE);
            CHECK(policy.move);
            CHECK(policy.copy);
            policy.chosen = core::Effect::Copy;
            effect = DROPEFFECT_COPY;
            CHECK(target.DragOver(0, point, &effect) == S_OK);
            CHECK(effect == DROPEFFECT_COPY);
            CHECK(target.DragLeave() == S_OK);
            policy.chosen = core::Effect::Move;
            effect = DROPEFFECT_MOVE;
            CHECK(target.Drop(nullptr, 0, point, &effect) == S_OK);
            CHECK(effect == DROPEFFECT_MOVE);
            CHECK(target.DragEnter(nullptr, 0, point, nullptr) == E_INVALIDARG);
            CHECK(target.DragOver(0, point, nullptr) == E_INVALIDARG);
            CHECK(target.Drop(nullptr, 0, point, nullptr) == E_INVALIDARG);
        }
    }

} // namespace burlak::drag
