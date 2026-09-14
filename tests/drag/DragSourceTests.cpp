#include "drag/DragSource.hpp"

#include <doctest/doctest.h>

namespace burlak::drag
{

    namespace
    {

        class Screen final : public core::IScreen
        {
          public:
            std::optional<core::Point> point{core::Point{5, 6}};
            core::NativeWindow root{20};

            [[nodiscard]] std::optional<core::Point> cursor() override
            {
                return point;
            }
            [[nodiscard]] bool buttonDown(core::Button) override
            {
                return false;
            }
            [[nodiscard]] core::NativeWindow windowAt(core::Point) override
            {
                return root;
            }
            [[nodiscard]] core::NativeWindow consoleWindow() override
            {
                return 10;
            }
            [[nodiscard]] core::NativeWindow hostWindowHandle() override
            {
                return 10;
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

            [[nodiscard]] bool extract() override
            {
                ++calls;
                return succeeds;
            }
            void retain() override
            {
            }
            void cleanup() override
            {
            }
        };

    } // namespace

    TEST_SUITE("drag source")
    {
        TEST_CASE("COM identity and reference count stay stack-owned")
        {
            core::ReleasePolicy policy;
            Screen screen;
            Extraction extraction;
            DragSource source{policy, screen, extraction, core::Button::Left, 30, false};

            CHECK(source.QueryInterface(IID_IUnknown, nullptr) == E_POINTER);
            void *object{};
            CHECK(source.QueryInterface(IID_IUnknown, &object) == S_OK);
            CHECK(object == static_cast<IDropSource *>(&source));
            CHECK(source.Release() == 1);
            object = nullptr;
            CHECK(source.QueryInterface(IID_IDropSource, &object) == S_OK);
            CHECK(source.Release() == 1);
            const GUID unknown{0x12345678, 0, 0, {}};
            CHECK(source.QueryInterface(unknown, &object) == E_NOINTERFACE);
            CHECK(object == nullptr);
            CHECK(source.AddRef() == 2);
            CHECK(source.Release() == 1);
        }

        TEST_CASE("button state and Escape map to OLE continuation outcomes")
        {
            core::ReleasePolicy policy;
            Screen screen;
            Extraction extraction;
            DragSource left{policy, screen, extraction, core::Button::Left, 30, false};
            DragSource right{policy, screen, extraction, core::Button::Right, 30, false};

            CHECK(left.QueryContinueDrag(TRUE, MK_LBUTTON) == DRAGDROP_S_CANCEL);
            CHECK(left.QueryContinueDrag(FALSE, MK_LBUTTON) == S_OK);
            CHECK(left.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_DROP);
            CHECK(right.QueryContinueDrag(FALSE, MK_RBUTTON) == S_OK);
            CHECK(right.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_DROP);
        }

        TEST_CASE("accepted plugin payloads extract for foreign targets but not the own overlay")
        {
            core::ReleasePolicy policy;
            Screen screen;
            Extraction extraction;
            DragSource source{policy, screen, extraction, core::Button::Left, 30, true};
            CHECK(source.GiveFeedback(DROPEFFECT_COPY) == DRAGDROP_S_USEDEFAULTCURSORS);
            CHECK(source.lastEffect() == core::Effect::Copy);

            SUBCASE("foreign target")
            {
                CHECK(source.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_DROP);
                CHECK(source.extractionRan());
                CHECK(extraction.calls == 1);
            }
            SUBCASE("extraction failure")
            {
                extraction.succeeds = false;
                CHECK(source.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_CANCEL);
                CHECK_FALSE(source.extractionRan());
                CHECK(extraction.calls == 1);
            }
            SUBCASE("own overlay")
            {
                screen.root = 30;
                CHECK(source.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_DROP);
                CHECK_FALSE(source.extractionRan());
                CHECK(extraction.calls == 0);
            }
            SUBCASE("cursor unavailable")
            {
                screen.point.reset();
                CHECK(source.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_DROP);
                CHECK(source.extractionRan());
            }
        }

        TEST_CASE("feedback follows move copy link priority and blocks links for extracted payloads")
        {
            core::ReleasePolicy policy;
            Screen screen;
            Extraction extraction;
            DragSource ordinary{policy, screen, extraction, core::Button::Left, 30, false};
            CHECK(ordinary.GiveFeedback(DROPEFFECT_MOVE | DROPEFFECT_COPY | DROPEFFECT_LINK) ==
                  DRAGDROP_S_USEDEFAULTCURSORS);
            CHECK(ordinary.lastEffect() == core::Effect::Move);
            CHECK(ordinary.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_DROP);
            CHECK(ordinary.GiveFeedback(DROPEFFECT_COPY | DROPEFFECT_LINK) == DRAGDROP_S_USEDEFAULTCURSORS);
            CHECK(ordinary.lastEffect() == core::Effect::Copy);
            CHECK(ordinary.GiveFeedback(DROPEFFECT_LINK) == DRAGDROP_S_USEDEFAULTCURSORS);
            CHECK(ordinary.lastEffect() == core::Effect::Link);
            CHECK(ordinary.GiveFeedback(DROPEFFECT_NONE) == DRAGDROP_S_USEDEFAULTCURSORS);
            CHECK(ordinary.lastEffect() == core::Effect::None);

            DragSource extracted{policy, screen, extraction, core::Button::Left, 30, true};
            CHECK(extracted.GiveFeedback(DROPEFFECT_LINK) == DRAGDROP_S_USEDEFAULTCURSORS);
            CHECK(extracted.lastEffect() == core::Effect::None);
        }
    }

} // namespace burlak::drag
