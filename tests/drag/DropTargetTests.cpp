#include "adapters/shell/Shell.hpp"
#include "drag/DropTarget.hpp"

#include "../core/Fakes.hpp"
#include "core/Session.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <vector>

namespace burlak::drag
{

    namespace
    {

        class DropSession final : public core::IDropSession
        {
          public:
            core::Effect hovered{core::Effect::None};
            core::Effect dropped{core::Effect::None};
            mutable core::Point hoverPoint{};
            core::Point dropPoint{};
            mutable bool hoverShift{};
            bool dropShift{};
            int drops{};

            void prepare(core::DropContext) override
            {
            }

            [[nodiscard]] core::Effect effect(core::Point point, bool shift) const override
            {
                hoverPoint = point;
                hoverShift = shift;
                return hovered;
            }

            [[nodiscard]] core::Effect drop(core::Point point, bool shift) override
            {
                dropPoint = point;
                dropShift = shift;
                ++drops;
                return dropped;
            }
        };

        class Screen final : public core::IScreen
        {
          public:
            [[nodiscard]] std::optional<core::Point> cursor() override
            {
                return std::nullopt;
            }
            [[nodiscard]] bool buttonDown(core::Button) override
            {
                return true;
            }
            [[nodiscard]] std::optional<core::HostWindow> hostWindow() override
            {
                return core::HostWindow{1, {0, 0, 80, 25}, false};
            }
            [[nodiscard]] std::expected<core::CellGeometry, core::Error> cellGeometry() override
            {
                return core::CellGeometry{{0, 0}, 1, 1};
            }
        };

        class Input final : public core::IInput
        {
          public:
            std::vector<core::MouseEvent> replayed;

            void release(core::Button) override
            {
            }
            void press(core::Button) override
            {
            }
            [[nodiscard]] core::ReplayOutcome replay(std::span<const core::MouseEvent> events) override
            {
                replayed.assign(events.begin(), events.end());
                const auto size = static_cast<std::uint32_t>(events.size());
                return {true, size, size};
            }
        };

        class Tool final : public core::IDragTool
        {
          public:
            explicit Tool(core::Session &session) : session_{session}
            {
            }

            [[nodiscard]] bool start() override
            {
                return true;
            }
            [[nodiscard]] bool prepare(std::span<const std::wstring> paths, core::Button,
                                       core::DropContext context) override
            {
                auto result = adapters::shell::makeDataObject(paths);
                if (!result) {
                    return false;
                }
                data = std::move(*result);
                session_.prepare(context);
                return true;
            }
            [[nodiscard]] bool showAndArm() override
            {
                return true;
            }
            void abort() override
            {
            }
            [[nodiscard]] bool active() const override
            {
                return false;
            }
            void stop() override
            {
            }

            adapters::shell::DataObject data;

          private:
            core::Session &session_;
        };

        class OleApartment final
        {
          public:
            OleApartment() : initialized_{SUCCEEDED(OleInitialize(nullptr))}
            {
            }
            ~OleApartment()
            {
                if (initialized_) {
                    OleUninitialize();
                }
            }
            [[nodiscard]] bool initialized() const
            {
                return initialized_;
            }

          private:
            bool initialized_{};
        };

        class TemporaryFile final
        {
          public:
            TemporaryFile()
                : root_{std::filesystem::temp_directory_path() /
                        (L"burlak-drop-target-" + std::to_wstring(GetCurrentProcessId()))},
                  path_{root_ / L"one.txt"}
            {
                std::filesystem::create_directories(root_);
                std::ofstream stream{path_};
            }
            ~TemporaryFile()
            {
                std::error_code ignored;
                std::filesystem::remove_all(root_, ignored);
            }

            [[nodiscard]] const std::filesystem::path &root() const
            {
                return root_;
            }

            [[nodiscard]] const std::filesystem::path &path() const
            {
                return path_;
            }

          private:
            std::filesystem::path root_;
            std::filesystem::path path_;
        };

    } // namespace

    TEST_SUITE("drop target")
    {
        TEST_CASE("COM identity and hover methods forward point and Shift to the session")
        {
            DropSession session;
            DropTarget target{session};
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

            session.hovered = core::Effect::Copy;
            DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
            CHECK(target.DragEnter(nullptr, 0, {120, 140}, &effect) == S_OK);
            CHECK(effect == DROPEFFECT_COPY);
            CHECK(session.hoverPoint == core::Point{120, 140});
            CHECK_FALSE(session.hoverShift);

            session.hovered = core::Effect::Move;
            effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
            CHECK(target.DragOver(MK_SHIFT, {121, 141}, &effect) == S_OK);
            CHECK(effect == DROPEFFECT_MOVE);
            CHECK(session.hoverPoint == core::Point{121, 141});
            CHECK(session.hoverShift);
            CHECK(target.DragLeave() == S_OK);

            CHECK(target.DragEnter(nullptr, 0, {}, nullptr) == E_INVALIDARG);
            CHECK(target.DragOver(0, {}, nullptr) == E_INVALIDARG);
            CHECK(target.Drop(nullptr, 0, {}, nullptr) == E_INVALIDARG);
        }

        TEST_CASE("a drop over the fake passive panel replays Far's two records in order")
        {
            OleApartment ole;
            REQUIRE(ole.initialized());
            TemporaryFile file;
            tests::Panels panels;
            panels.panels[0] = core::PanelInfo{
                .visible = true, .realNames = true, .filePanel = true, .rect = {0, 0, 39, 24}, .handle = 11};
            panels.panels[1] = core::PanelInfo{
                .visible = true, .plugin = true, .filePanel = true, .rect = {40, 0, 79, 24}, .handle = 22};
            panels.directories[0] = file.root().wstring();
            panels.items[0] = {{.name = file.path().filename().wstring()}};
            Screen screen;
            Input input;
            tests::Host host;
            core::Session session{panels, host, screen, input};
            Tool tool{session};
            REQUIRE(session.begin(tool, core::DragStart{core::Button::Left, {5, 5}}));
            REQUIRE(tool.data.Get() != nullptr);
            DropTarget target{session};

            DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
            CHECK(target.DragEnter(tool.data.Get(), 0, {45, 5}, &effect) == S_OK);
            CHECK(effect == DROPEFFECT_COPY);
            CHECK(target.DragLeave() == S_OK);
            CHECK(target.Drop(tool.data.Get(), MK_SHIFT, {45, 6}, &effect) == S_OK);
            CHECK(effect == DROPEFFECT_MOVE);
            CHECK(input.replayed.empty());
            CHECK(host.synchros == 1);
            session.synchro();
            CHECK(input.replayed ==
                  std::vector<core::MouseEvent>{core::MouseEvent{.at = {5, 5}, .left = true, .mods = {.shift = true}},
                                                core::MouseEvent{.at = {45, 6}, .mods = {.shift = true}}});
        }

        TEST_CASE("Drop asks the session's drop path rather than hover feedback")
        {
            DropSession session;
            session.hovered = core::Effect::Copy;
            session.dropped = core::Effect::Move;
            DropTarget target{session};
            DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;

            CHECK(target.Drop(nullptr, MK_SHIFT, {7, 8}, &effect) == S_OK);
            CHECK(effect == DROPEFFECT_MOVE);
            CHECK(session.drops == 1);
            CHECK(session.dropPoint == core::Point{7, 8});
            CHECK(session.dropShift);
        }
    }

} // namespace burlak::drag
