#include "adapters/shell/Shell.hpp"
#include "drag/DropTarget.hpp"

#include "../core/Fakes.hpp"
#include "core/Session.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <new>
#include <stdexcept>
#include <vector>

namespace burlak::drag
{

    namespace
    {
        adapters::shell::DropData &dropData()
        {
            static adapters::shell::DropData bridge;
            return bridge;
        }

        class ThrowingDropData final : public core::IDropData
        {
          public:
            enum class Operation : std::uint8_t
            {
                Offers,
                Paths,
                Performed
            } operation{Operation::Offers};
            bool allocation{true};

            [[noreturn]] void fail() const
            {
                if (allocation) {
                    throw std::bad_alloc{};
                }
                throw std::logic_error{"test invariant"};
            }

            [[nodiscard]] bool offersFileDrop(std::uintptr_t) const override
            {
                if (operation == Operation::Offers) {
                    fail();
                }
                return true;
            }
            [[nodiscard]] std::expected<std::vector<std::wstring>, core::Error> fileDropPaths(
                std::uintptr_t) const override
            {
                if (operation == Operation::Paths) {
                    fail();
                }
                return std::vector<std::wstring>{L"C:\\source.txt"};
            }
            [[nodiscard]] std::expected<void, core::Error> setPerformedEffect(std::uintptr_t, core::Effect) override
            {
                if (operation == Operation::Performed) {
                    fail();
                }
                return {};
            }
        };

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
            core::Effect received{core::Effect::None};
            core::ReceiveDropOutcome receiveOutcome{};
            core::NativeWindow owner{77};
            mutable int receiveHovers{};
            int receiveDrops{};
            std::optional<core::Effect> forcedReceiveEffect;
            std::function<void()> duringReceiveDrop;
            mutable std::vector<core::AllowedEffects> allowedEffects;
            bool throwOnSourceEffect{};

            int sourceEnds{};

            void prepare(core::DropContext) override
            {
            }

            void endSource() override
            {
                ++sourceEnds;
            }

            [[nodiscard]] core::Effect effect(core::Point point, bool shift) const override
            {
                if (throwOnSourceEffect) {
                    throw std::logic_error{"source effect invariant"};
                }
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

            [[nodiscard]] bool requestReceiveSnapshot(core::Point) override
            {
                return true;
            }

            [[nodiscard]] bool requestReceiveRefresh(core::Point) override
            {
                return true;
            }

            void prepareReceive(core::ReceiveSnapshot) override
            {
            }

            void cancelReceive() override
            {
            }

            [[nodiscard]] core::Effect receiveEffect(core::Point, bool shift,
                                                     core::AllowedEffects allowed) const override
            {
                allowedEffects.push_back(allowed);
                ++receiveHovers;
                if (forcedReceiveEffect) {
                    return *forcedReceiveEffect;
                }
                if (shift && allowed.move) {
                    return core::Effect::Move;
                }
                if (allowed.copy) {
                    return core::Effect::Copy;
                }
                return allowed.move ? core::Effect::Move : core::Effect::None;
            }

            [[nodiscard]] core::NativeWindow receiveOwner() const override
            {
                return owner;
            }

            [[nodiscard]] core::ReceiveDropOutcome receiveDrop(std::span<const std::wstring>, core::Point,
                                                               core::Effect effect) override
            {
                ++receiveDrops;
                received = effect;
                if (duringReceiveDrop) {
                    duringReceiveDrop();
                }
                return receiveOutcome;
            }
        };

        class Menu final : public core::IDropMenu
        {
          public:
            core::DropMenuChoice choice{core::DropMenuChoice::Cancel};
            int calls{};
            core::NativeWindow owner{};
            core::AllowedEffects allowed{};
            std::function<void()> duringChoose;

            [[nodiscard]] core::DropMenuChoice choose(core::NativeWindow selectedOwner, core::Point,
                                                      core::AllowedEffects selectedAllowed) override
            {
                ++calls;
                owner = selectedOwner;
                allowed = selectedAllowed;
                if (duringChoose) {
                    duringChoose();
                }
                return choice;
            }
        };

        class ReceiveLifecycle final : public IReceiveLifecycle
        {
          public:
            enum class Stage : std::uint8_t
            {
                Inactive,
                Armed,
                Entered,
                Dropping
            };

            bool receiving{};
            int finishes{};
            int leaves{};
            int refreshes{};
            bool refreshSucceeds{true};
            bool throwOnLeave{};
            Stage stage{Stage::Inactive};
            core::NativeWindow owner{55};
            bool source{};

            void arm()
            {
                receiving = true;
                stage = Stage::Armed;
            }

            [[nodiscard]] bool sourceMode() const noexcept override
            {
                return source;
            }

            [[nodiscard]] core::NativeWindow menuOwner() const noexcept override
            {
                return owner;
            }

            [[nodiscard]] bool enterReceive() override
            {
                if (stage != Stage::Armed) {
                    return false;
                }
                stage = Stage::Entered;
                return true;
            }

            void leaveReceive() override
            {
                ++leaves;
                if (throwOnLeave) {
                    throw std::logic_error{"leave invariant"};
                }
                if (stage == Stage::Entered) {
                    stage = Stage::Armed;
                }
            }

            [[nodiscard]] bool beginReceiveDrop() override
            {
                if (stage != Stage::Entered) {
                    return false;
                }
                stage = Stage::Dropping;
                return true;
            }

            [[nodiscard]] bool refreshReceive(core::Point) override
            {
                ++refreshes;
                return refreshSucceeds;
            }

            [[nodiscard]] bool receiveMode() const noexcept override
            {
                return receiving;
            }

            void finishReceive() noexcept override
            {
                ++finishes;
                receiving = false;
                stage = Stage::Inactive;
            }
        };

        class InvalidFileData final : public IDataObject
        {
          public:
            enum class Mode : std::uint8_t
            {
                Missing,
                RetrievalFailure,
                InvalidMedium,
                EmptyGlobal
            };

            explicit InvalidFileData(Mode mode) : mode_{mode}
            {
            }

            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **object) override
            {
                if (object == nullptr) {
                    return E_POINTER;
                }
                *object = nullptr;
                if (id != IID_IUnknown && id != IID_IDataObject) {
                    return E_NOINTERFACE;
                }
                *object = static_cast<IDataObject *>(this);
                AddRef();
                return S_OK;
            }
            ULONG STDMETHODCALLTYPE AddRef() override
            {
                return ++references_;
            }
            ULONG STDMETHODCALLTYPE Release() override
            {
                return --references_;
            }
            HRESULT STDMETHODCALLTYPE GetData(FORMATETC *, STGMEDIUM *medium) override
            {
                if (mode_ == Mode::InvalidMedium) {
                    *medium = {};
                    return S_OK;
                }
                if (mode_ == Mode::EmptyGlobal) {
                    *medium = {};
                    medium->tymed = TYMED_HGLOBAL;
                    return S_OK;
                }
                return DV_E_FORMATETC;
            }
            HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override
            {
                return E_NOTIMPL;
            }
            HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *) override
            {
                return mode_ == Mode::Missing ? DV_E_FORMATETC : S_OK;
            }
            HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *, FORMATETC *) override
            {
                return E_NOTIMPL;
            }
            HRESULT STDMETHODCALLTYPE SetData(FORMATETC *, STGMEDIUM *, BOOL) override
            {
                return E_NOTIMPL;
            }
            HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC **) override
            {
                return E_NOTIMPL;
            }
            HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override
            {
                return OLE_E_ADVISENOTSUPPORTED;
            }
            HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override
            {
                return OLE_E_ADVISENOTSUPPORTED;
            }
            HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **) override
            {
                return OLE_E_ADVISENOTSUPPORTED;
            }

          private:
            Mode mode_;
            ULONG references_{1};
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
            [[nodiscard]] core::NativeWindow windowAt(core::Point) override
            {
                return 0;
            }
            [[nodiscard]] core::NativeWindow consoleWindow() override
            {
                return 1;
            }
            [[nodiscard]] core::NativeWindow hostWindowHandle() override
            {
                return 1;
            }
            [[nodiscard]] std::optional<core::HostWindow> hostWindow() override
            {
                return core::HostWindow{1, {0, 0, 80, 25}, false};
            }
            [[nodiscard]] std::optional<core::HostWindow> hostWindowAt(core::Point) override
            {
                return hostWindow();
            }
            [[nodiscard]] std::expected<core::CellGeometry, core::Error> cellGeometry() override
            {
                return core::CellGeometry{{0, 0}, 1, 1};
            }
            [[nodiscard]] std::expected<core::CellGeometry, core::Error> cellGeometryAt(core::Point) override
            {
                return cellGeometry();
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
            [[nodiscard]] bool prepare(std::span<const std::wstring> paths, core::Button, bool,
                                       core::DropContext context) override
            {
                auto result = adapters::shell::makeDataObject(paths);
                if (!result) {
                    return false;
                }
                data = std::move(result->data);
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
            Menu menu;
            ReceiveLifecycle lifecycle;
            lifecycle.source = true;
            DropTarget target{session, dropData(), menu, lifecycle};
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
            panels.directories[1] = L"D:\\target";
            panels.items[0] = {{.name = file.path().filename().wstring()}};
            Screen screen;
            Input input;
            tests::Host host;
            tests::Files files;
            tests::Shell shell;
            core::Session session{panels, host, screen, input, files, shell};
            Tool tool{session};
            REQUIRE(session.begin(tool, core::DragStart{core::Button::Left, {5, 5}}));
            REQUIRE(tool.data.Get() != nullptr);
            Menu menu;
            ReceiveLifecycle lifecycle;
            lifecycle.source = true;
            DropTarget target{session, dropData(), menu, lifecycle};

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
            Menu menu;
            ReceiveLifecycle lifecycle;
            lifecycle.source = true;
            DropTarget target{session, dropData(), menu, lifecycle};
            DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;

            CHECK(target.Drop(nullptr, MK_SHIFT, {7, 8}, &effect) == S_OK);
            CHECK(effect == DROPEFFECT_MOVE);
            CHECK(session.drops == 1);
            CHECK(session.dropPoint == core::Point{7, 8});
            CHECK(session.dropShift);
        }

        TEST_CASE("receive mode copies or optimizes a move from a real shell data object")
        {
            OleApartment ole;
            REQUIRE(ole.initialized());
            TemporaryFile source;
            const auto destination = std::filesystem::temp_directory_path() /
                                     (L"burlak-drop-receive-" + std::to_wstring(GetCurrentProcessId()));
            std::error_code ignored;
            std::filesystem::remove_all(destination, ignored);
            std::filesystem::create_directories(destination);
            const std::vector<std::wstring> paths{source.path().wstring()};
            auto data = adapters::shell::makeDataObject(paths);
            REQUIRE(data.has_value());

            tests::Panels panels;
            panels.panels[0] = core::PanelInfo{
                .visible = true, .realNames = true, .filePanel = true, .rect = {0, 0, 39, 24}, .handle = 11};
            panels.directories[0] = destination.wstring();
            Screen screen;
            Input input;
            tests::Host host;
            tests::Files files;
            adapters::shell::Shell shell;
            core::Session session{panels, host, screen, input, files, shell};
            session.prepareReceive(core::ReceiveSnapshot{.panelsWindow = true,
                                                         .panels = panels.panels,
                                                         .directories = panels.directories,
                                                         .host = core::HostWindow{1, {0, 0, 80, 25}, false},
                                                         .geometry = core::CellGeometry{{0, 0}, 1, 1}});
            Menu menu;
            ReceiveLifecycle lifecycle;
            lifecycle.arm();
            DropTarget target{session, dropData(), menu, lifecycle};
            DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
            bool moving{};

            SUBCASE("copy")
            {
                CHECK(target.DragEnter(data->data.Get(), 0, {5, 5}, &effect) == S_OK);
                CHECK(effect == DROPEFFECT_COPY);
            }
            SUBCASE("optimized move")
            {
                moving = true;
                CHECK(target.DragEnter(data->data.Get(), MK_SHIFT, {5, 5}, &effect) == S_OK);
                CHECK(effect == DROPEFFECT_MOVE);
            }

            CHECK(target.DragLeave() == S_OK);
            CHECK(lifecycle.finishes == 0);
            effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
            CHECK(target.DragEnter(data->data.Get(), moving ? MK_SHIFT : 0, {5, 5}, &effect) == S_OK);
            CHECK(target.Drop(data->data.Get(), moving ? MK_SHIFT : 0, {5, 5}, &effect) == S_OK);
            CHECK(effect == (moving ? DROPEFFECT_NONE : DROPEFFECT_COPY));
            CHECK(std::filesystem::exists(destination / L"one.txt"));
            CHECK(lifecycle.finishes == 1);
            CHECK(host.synchros == 1);
            session.synchro();
            CHECK(panels.updates == std::vector<core::PanelSide>{core::PanelSide::Active});

            const auto performedFormat = RegisterClipboardFormatW(CFSTR_PERFORMEDDROPEFFECT);
            FORMATETC performed{static_cast<CLIPFORMAT>(performedFormat), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            STGMEDIUM medium{};
            const auto status = data->data->GetData(&performed, &medium);
            CHECK((moving ? SUCCEEDED(status) : status == DV_E_FORMATETC));
            if (SUCCEEDED(status)) {
                const auto value = static_cast<const DWORD *>(GlobalLock(medium.hGlobal));
                REQUIRE(value != nullptr);
                CHECK(*value == DROPEFFECT_NONE);
                GlobalUnlock(medium.hGlobal);
                ReleaseStgMedium(&medium);
            }
            std::filesystem::remove_all(destination, ignored);
        }

        TEST_CASE("receive mode rejects missing file data and maps a right-button menu before Drop")
        {
            DropSession session;
            Menu menu;
            ReceiveLifecycle lifecycle;
            lifecycle.arm();
            DropTarget target{session, dropData(), menu, lifecycle};
            DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;

            CHECK(target.DragEnter(nullptr, MK_RBUTTON, {5, 5}, &effect) == S_OK);
            CHECK(target.DragEnter(nullptr, MK_RBUTTON, {5, 5}, nullptr) == E_INVALIDARG);
            CHECK(effect == DROPEFFECT_NONE);
            CHECK(target.DragOver(MK_RBUTTON, {5, 5}, &effect) == S_OK);
            CHECK(effect == DROPEFFECT_NONE);
            CHECK(target.Drop(nullptr, 0, {5, 5}, &effect) == S_OK);
            CHECK(effect == DROPEFFECT_NONE);
            CHECK(session.receiveDrops == 0);
            CHECK(lifecycle.finishes == 1);
        }

        TEST_CASE("receive mode rejects data that omits or cannot return CF_HDROP")
        {
            DropSession session;
            Menu menu;
            ReceiveLifecycle lifecycle;
            DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;

            for (const auto mode : {InvalidFileData::Mode::Missing, InvalidFileData::Mode::RetrievalFailure,
                                    InvalidFileData::Mode::InvalidMedium, InvalidFileData::Mode::EmptyGlobal}) {
                lifecycle.arm();
                effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
                InvalidFileData data{mode};
                DropTarget target{session, dropData(), menu, lifecycle};
                CHECK(target.DragEnter(&data, 0, {5, 5}, &effect) == S_OK);
                CHECK(effect == (mode == InvalidFileData::Mode::Missing ? DROPEFFECT_NONE : DROPEFFECT_COPY));
                CHECK(target.Drop(&data, 0, {5, 5}, &effect) == S_OK);
                CHECK(effect == DROPEFFECT_NONE);
            }
            CHECK(session.receiveDrops == 0);
            CHECK(lifecycle.finishes == 4);
        }

        TEST_CASE("receive mode does not read a valid file drop after hover becomes ineligible")
        {
            OleApartment ole;
            REQUIRE(ole.initialized());
            TemporaryFile file;
            const std::vector<std::wstring> paths{file.path().wstring()};
            auto data = adapters::shell::makeDataObject(paths);
            REQUIRE(data.has_value());
            DropSession session;
            session.forcedReceiveEffect = core::Effect::None;
            Menu menu;
            ReceiveLifecycle lifecycle;
            lifecycle.arm();
            DropTarget target{session, dropData(), menu, lifecycle};
            DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;

            CHECK(target.DragEnter(data->data.Get(), 0, {5, 5}, &effect) == S_OK);
            CHECK(effect == DROPEFFECT_NONE);
            CHECK(target.Drop(data->data.Get(), 0, {5, 5}, &effect) == S_OK);
            CHECK(effect == DROPEFFECT_NONE);
            CHECK(session.receiveDrops == 0);
        }

        TEST_CASE("receive Drop cancels when the Far-thread identity refresh fails")
        {
            OleApartment ole;
            REQUIRE(ole.initialized());
            TemporaryFile file;
            const std::vector<std::wstring> paths{file.path().wstring()};
            auto data = adapters::shell::makeDataObject(paths);
            REQUIRE(data.has_value());
            DropSession session;
            Menu menu;
            ReceiveLifecycle lifecycle;
            lifecycle.arm();
            lifecycle.refreshSucceeds = false;
            DropTarget target{session, dropData(), menu, lifecycle};
            DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;

            REQUIRE(target.DragEnter(data->data.Get(), 0, {5, 5}, &effect) == S_OK);
            CHECK(target.Drop(data->data.Get(), 0, {5, 5}, &effect) == S_OK);
            CHECK(effect == DROPEFFECT_NONE);
            CHECK(lifecycle.refreshes == 1);
            CHECK(session.receiveDrops == 0);
        }

        TEST_CASE("COM entry points contain allocation failures and broken-invariant exceptions")
        {
            for (const bool allocation : {true, false}) {
                for (const auto operation : {ThrowingDropData::Operation::Offers, ThrowingDropData::Operation::Paths,
                                             ThrowingDropData::Operation::Performed}) {
                    DropSession session;
                    if (operation == ThrowingDropData::Operation::Performed) {
                        session.receiveOutcome = {.returnedEffect = core::Effect::None, .setPerformedNone = true};
                    }
                    ThrowingDropData data;
                    data.operation = operation;
                    data.allocation = allocation;
                    Menu menu;
                    ReceiveLifecycle lifecycle;
                    lifecycle.arm();
                    DropTarget target{session, data, menu, lifecycle};
                    DWORD effect = DROPEFFECT_COPY;
                    const auto expected = allocation ? E_OUTOFMEMORY : E_UNEXPECTED;

                    if (operation == ThrowingDropData::Operation::Offers) {
                        CHECK(target.DragEnter(nullptr, 0, {5, 5}, &effect) == expected);
                    } else {
                        REQUIRE(target.DragEnter(nullptr, 0, {5, 5}, &effect) == S_OK);
                        CHECK(target.Drop(nullptr, 0, {5, 5}, &effect) == expected);
                    }
                    CHECK(effect == DROPEFFECT_NONE);
                    CHECK_FALSE(lifecycle.receiveMode());
                }
            }

            SUBCASE("DragLeave contains an invariant failure without an effect pointer")
            {
                DropSession session;
                Menu menu;
                ReceiveLifecycle lifecycle;
                lifecycle.arm();
                DropTarget target{session, dropData(), menu, lifecycle};
                DWORD effect = DROPEFFECT_COPY;
                REQUIRE(target.DragEnter(nullptr, 0, {5, 5}, &effect) == S_OK);
                lifecycle.throwOnLeave = true;
                CHECK(target.DragLeave() == E_UNEXPECTED);
                CHECK_FALSE(lifecycle.receiveMode());
            }
            SUBCASE("a source-mode invariant failure does not run receive cleanup")
            {
                DropSession session;
                session.throwOnSourceEffect = true;
                Menu menu;
                ReceiveLifecycle lifecycle;
                lifecycle.source = true;
                DropTarget target{session, dropData(), menu, lifecycle};
                DWORD effect = DROPEFFECT_COPY;
                CHECK(target.DragOver(0, {5, 5}, &effect) == E_UNEXPECTED);
                CHECK(effect == DROPEFFECT_NONE);
                CHECK(lifecycle.finishes == 0);
            }
            SUBCASE("a stray drop reaching the idle overlay never touches the session")
            {
                DropSession session;
                session.hovered = core::Effect::Copy;
                session.dropped = core::Effect::Move;
                session.throwOnSourceEffect = true; // proves session.effect is not consulted either
                Menu menu;
                ReceiveLifecycle lifecycle; // neither receiveMode nor sourceMode: the window is Idle
                DropTarget target{session, dropData(), menu, lifecycle};
                DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
                CHECK(target.DragOver(0, {45, 6}, &effect) == S_OK);
                CHECK(effect == DROPEFFECT_NONE);
                CHECK(target.Drop(nullptr, MK_SHIFT, {45, 6}, &effect) == S_OK);
                CHECK(effect == DROPEFFECT_NONE);
                CHECK(session.drops == 0);
                CHECK(lifecycle.finishes == 0);
            }
        }

        TEST_CASE("a right-button file drop uses the receiver's popup choice")
        {
            OleApartment ole;
            REQUIRE(ole.initialized());
            TemporaryFile file;
            const std::vector<std::wstring> paths{file.path().wstring()};
            auto data = adapters::shell::makeDataObject(paths);
            REQUIRE(data.has_value());
            DropSession session;
            Menu menu;
            ReceiveLifecycle lifecycle;
            lifecycle.arm();
            DropTarget target{session, dropData(), menu, lifecycle};
            DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
            core::Effect expected{core::Effect::None};

            SUBCASE("copy")
            {
                menu.choice = core::DropMenuChoice::Copy;
                session.receiveOutcome = {core::Effect::Copy, false};
                expected = core::Effect::Copy;
            }
            SUBCASE("move")
            {
                menu.choice = core::DropMenuChoice::Move;
                session.receiveOutcome = {core::Effect::None, true};
                expected = core::Effect::Move;
            }
            SUBCASE("cancel")
            {
                menu.choice = core::DropMenuChoice::Cancel;
            }

            CHECK(target.DragEnter(data->data.Get(), MK_RBUTTON, {5, 5}, &effect) == S_OK);
            CHECK(target.Drop(data->data.Get(), 0, {5, 5}, &effect) == S_OK);
            CHECK(menu.calls == 1);
            // TrackPopupMenu accepts only an owner of the calling thread, so the popup belongs to the tool window
            // while the host (session.owner) stays the owner of IFileOperation.
            CHECK(menu.owner == lifecycle.owner);
            CHECK(menu.owner != session.owner);
            CHECK(session.received == expected);
            CHECK(session.receiveDrops == (expected == core::Effect::None ? 0 : 1));
            CHECK(lifecycle.finishes == 1);
        }

        TEST_CASE("receive hover drop and menu choices stay within the DragEnter effect mask")
        {
            OleApartment ole;
            REQUIRE(ole.initialized());
            TemporaryFile file;
            const std::vector<std::wstring> paths{file.path().wstring()};
            auto data = adapters::shell::makeDataObject(paths);
            REQUIRE(data.has_value());
            DropSession session;
            Menu menu;
            ReceiveLifecycle lifecycle;
            lifecycle.arm();
            DropTarget target{session, dropData(), menu, lifecycle};
            DWORD effect{};

            SUBCASE("copy-only source with Shift")
            {
                effect = DROPEFFECT_COPY;
                session.receiveOutcome = {core::Effect::Copy, false};
                CHECK(target.DragEnter(data->data.Get(), MK_SHIFT, {5, 5}, &effect) == S_OK);
                CHECK(effect == DROPEFFECT_COPY);
                effect = DROPEFFECT_MOVE;
                CHECK(target.DragOver(MK_SHIFT, {5, 5}, &effect) == S_OK);
                CHECK(effect == DROPEFFECT_COPY);
                CHECK(target.Drop(data->data.Get(), MK_SHIFT, {5, 5}, &effect) == S_OK);
                CHECK(effect == DROPEFFECT_COPY);
                CHECK(session.received == core::Effect::Copy);
                CHECK(session.allowedEffects == std::vector<core::AllowedEffects>(3, {.copy = true, .move = false}));
            }
            SUBCASE("move-only source without Shift")
            {
                effect = DROPEFFECT_MOVE;
                session.receiveOutcome = {core::Effect::None, true};
                CHECK(target.DragEnter(data->data.Get(), 0, {5, 5}, &effect) == S_OK);
                CHECK(effect == DROPEFFECT_MOVE);
                CHECK(target.Drop(data->data.Get(), 0, {5, 5}, &effect) == S_OK);
                CHECK(effect == DROPEFFECT_NONE);
                CHECK(session.received == core::Effect::Move);
            }
            SUBCASE("right-menu choice not allowed by the source")
            {
                effect = DROPEFFECT_COPY;
                menu.choice = core::DropMenuChoice::Move;
                CHECK(target.DragEnter(data->data.Get(), MK_RBUTTON, {5, 5}, &effect) == S_OK);
                CHECK(target.Drop(data->data.Get(), 0, {5, 5}, &effect) == S_OK);
                CHECK(effect == DROPEFFECT_NONE);
                CHECK(menu.allowed == core::AllowedEffects{.copy = true, .move = false});
                CHECK(session.receiveDrops == 0);
            }
        }

        TEST_CASE("a cancelled right-button drag does not turn the next left-button drop into a menu drop")
        {
            OleApartment ole;
            REQUIRE(ole.initialized());
            TemporaryFile file;
            const std::vector<std::wstring> paths{file.path().wstring()};
            auto data = adapters::shell::makeDataObject(paths);
            REQUIRE(data.has_value());
            DropSession session;
            session.receiveOutcome = {core::Effect::Copy, false};
            Menu menu;
            ReceiveLifecycle lifecycle;
            lifecycle.arm();
            DropTarget target{session, dropData(), menu, lifecycle};
            DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;

            CHECK(target.DragEnter(data->data.Get(), MK_RBUTTON, {5, 5}, &effect) == S_OK);
            CHECK(target.DragLeave() == S_OK);
            lifecycle.finishReceive();
            lifecycle.arm();
            CHECK(target.DragEnter(data->data.Get(), 0, {5, 5}, &effect) == S_OK);
            CHECK(target.Drop(data->data.Get(), 0, {5, 5}, &effect) == S_OK);
            CHECK(menu.calls == 0);
            CHECK(session.receiveDrops == 1);
            CHECK(session.received == core::Effect::Copy);
        }

        TEST_CASE("receive lifecycle moves through armed entered dropping and rejects re-entry")
        {
            OleApartment ole;
            REQUIRE(ole.initialized());
            TemporaryFile file;
            const std::vector<std::wstring> paths{file.path().wstring()};
            auto data = adapters::shell::makeDataObject(paths);
            REQUIRE(data.has_value());
            DropSession session;
            session.receiveOutcome = {core::Effect::Copy, false};
            Menu menu;
            ReceiveLifecycle lifecycle;
            lifecycle.arm();
            DropTarget target{session, dropData(), menu, lifecycle};
            DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;

            CHECK(target.DragEnter(data->data.Get(), 0, {5, 5}, &effect) == S_OK);
            CHECK(lifecycle.stage == ReceiveLifecycle::Stage::Entered);
            CHECK(target.DragLeave() == S_OK);
            CHECK(lifecycle.stage == ReceiveLifecycle::Stage::Armed);
            CHECK(target.DragEnter(data->data.Get(), 0, {5, 5}, &effect) == S_OK);
            CHECK(lifecycle.stage == ReceiveLifecycle::Stage::Entered);
            session.duringReceiveDrop = [&] {
                CHECK(lifecycle.stage == ReceiveLifecycle::Stage::Dropping);
                DWORD secondEffect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
                CHECK(target.DragEnter(data->data.Get(), 0, {5, 5}, &secondEffect) == S_OK);
                CHECK(secondEffect == DROPEFFECT_NONE);
            };
            CHECK(target.Drop(data->data.Get(), 0, {5, 5}, &effect) == S_OK);
            CHECK(effect == DROPEFFECT_COPY);
            CHECK(lifecycle.stage == ReceiveLifecycle::Stage::Inactive);
        }

        TEST_CASE("a second drag arriving while the first still lingers in Entered is refused throughout")
        {
            OleApartment ole;
            REQUIRE(ole.initialized());
            TemporaryFile file;
            const std::vector<std::wstring> paths{file.path().wstring()};
            auto data = adapters::shell::makeDataObject(paths);
            REQUIRE(data.has_value());
            DropSession session;
            session.receiveOutcome = {core::Effect::Copy, false};
            Menu menu;
            ReceiveLifecycle lifecycle;
            lifecycle.arm();
            DropTarget target{session, dropData(), menu, lifecycle};

            // Drag #1: a move-capable source enters, releases the button, and then dies without Drop or DragLeave.
            DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
            REQUIRE(target.DragEnter(data->data.Get(), MK_RBUTTON, {5, 5}, &effect) == S_OK);
            REQUIRE(lifecycle.stage == ReceiveLifecycle::Stage::Entered);
            const auto hoversAfterFirst = session.receiveHovers;

            // Drag #2: a copy-only source (Explorer) arrives while the overlay lingers. Its DragEnter is refused, and
            // nothing of drag #1 (its mask, its file data, its right button) may leak into drag #2's hover or drop.
            effect = DROPEFFECT_COPY;
            CHECK(target.DragEnter(data->data.Get(), MK_SHIFT, {5, 5}, &effect) == S_OK);
            CHECK(effect == DROPEFFECT_NONE);
            effect = DROPEFFECT_COPY;
            CHECK(target.DragOver(MK_SHIFT, {5, 5}, &effect) == S_OK);
            CHECK(effect == DROPEFFECT_NONE);
            effect = DROPEFFECT_COPY;
            CHECK(target.Drop(data->data.Get(), MK_SHIFT, {5, 5}, &effect) == S_OK);
            CHECK(effect == DROPEFFECT_NONE);
            CHECK(session.receiveHovers == hoversAfterFirst);
            CHECK(session.receiveDrops == 0);
            CHECK(menu.calls == 0);
            CHECK(lifecycle.stage == ReceiveLifecycle::Stage::Entered);
            CHECK(lifecycle.finishes == 0);

            // Only one OLE drag exists per desktop, so the next DragLeave in receive mode belongs to the only live
            // drag and must end the lingering Entered state even though the target's entry was already forgotten;
            // otherwise the overlay would swallow panel clicks until the ceiling.
            CHECK(target.DragLeave() == S_OK);
            CHECK(lifecycle.leaves == 1);
            CHECK(lifecycle.stage == ReceiveLifecycle::Stage::Armed);
            effect = DROPEFFECT_COPY;
            CHECK(target.DragEnter(data->data.Get(), 0, {5, 5}, &effect) == S_OK);
            CHECK(effect == DROPEFFECT_COPY);
            CHECK(lifecycle.stage == ReceiveLifecycle::Stage::Entered);
        }

        TEST_CASE("the first source's DragLeave ends a lingering receive after a second drag was refused")
        {
            OleApartment ole;
            REQUIRE(ole.initialized());
            TemporaryFile file;
            const std::vector<std::wstring> paths{file.path().wstring()};
            auto data = adapters::shell::makeDataObject(paths);
            REQUIRE(data.has_value());
            DropSession session;
            Menu menu;
            ReceiveLifecycle lifecycle;
            lifecycle.arm();
            DropTarget target{session, dropData(), menu, lifecycle};
            DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
            REQUIRE(target.DragEnter(data->data.Get(), 0, {5, 5}, &effect) == S_OK);
            effect = DROPEFFECT_COPY;
            REQUIRE(target.DragEnter(data->data.Get(), 0, {5, 5}, &effect) == S_OK);
            REQUIRE(effect == DROPEFFECT_NONE);

            // The first source cancels (Escape, extraction failed): its DragLeave arrives with the entry forgotten.
            CHECK(target.DragLeave() == S_OK);
            CHECK(lifecycle.leaves == 1);
            CHECK(lifecycle.stage == ReceiveLifecycle::Stage::Armed);
            CHECK(lifecycle.finishes == 0);
        }

        TEST_CASE("a refused DragEnter pumped from inside the right-button menu does not mask the chosen effect")
        {
            OleApartment ole;
            REQUIRE(ole.initialized());
            TemporaryFile file;
            const std::vector<std::wstring> paths{file.path().wstring()};
            auto data = adapters::shell::makeDataObject(paths);
            REQUIRE(data.has_value());
            DropSession session;
            session.receiveOutcome = {core::Effect::Copy, false};
            Menu menu;
            menu.choice = core::DropMenuChoice::Copy;
            ReceiveLifecycle lifecycle;
            lifecycle.arm();
            DropTarget target{session, dropData(), menu, lifecycle};
            // TrackPopupMenu pumps COM: another source's DragEnter can arrive while the menu is up and is refused in
            // Dropping, which forgets the target's entry (including the mask) under the menu's feet.
            menu.duringChoose = [&] {
                DWORD nested = DROPEFFECT_COPY | DROPEFFECT_MOVE;
                CHECK(target.DragEnter(data->data.Get(), 0, {5, 5}, &nested) == S_OK);
                CHECK(nested == DROPEFFECT_NONE);
            };
            DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
            REQUIRE(target.DragEnter(data->data.Get(), MK_RBUTTON, {5, 5}, &effect) == S_OK);
            CHECK(target.Drop(data->data.Get(), 0, {5, 5}, &effect) == S_OK);
            CHECK(menu.calls == 1);
            CHECK(effect == DROPEFFECT_COPY);
            CHECK(session.received == core::Effect::Copy);
            CHECK(session.receiveDrops == 1);
            CHECK(lifecycle.stage == ReceiveLifecycle::Stage::Inactive);
        }

        TEST_CASE("a drop carrying a stale entry after the overlay was torn down and re-armed is refused")
        {
            OleApartment ole;
            REQUIRE(ole.initialized());
            TemporaryFile file;
            const std::vector<std::wstring> paths{file.path().wstring()};
            auto data = adapters::shell::makeDataObject(paths);
            REQUIRE(data.has_value());
            DropSession session;
            session.receiveOutcome = {core::Effect::Copy, false};
            Menu menu;
            ReceiveLifecycle lifecycle;
            lifecycle.arm();
            DropTarget target{session, dropData(), menu, lifecycle};
            DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
            REQUIRE(target.DragEnter(data->data.Get(), 0, {5, 5}, &effect) == S_OK);

            // The source dies without DragLeave; the window times out to idle and later arms for a new drag. The
            // target's entry flag from the dead drag must not let a Drop take the Dropping transition.
            lifecycle.finishReceive();
            lifecycle.arm();
            effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
            CHECK(target.Drop(data->data.Get(), 0, {5, 5}, &effect) == S_OK);
            CHECK(effect == DROPEFFECT_NONE);
            CHECK(session.receiveDrops == 0);
            CHECK(lifecycle.stage == ReceiveLifecycle::Stage::Armed);
            CHECK(lifecycle.finishes == 1);

            // A DragLeave that reaches the target while the window is idle still forgets the dead drag's entry, so
            // the next armed receive cannot report hover feedback under that drag's mask.
            effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
            REQUIRE(target.DragEnter(data->data.Get(), 0, {5, 5}, &effect) == S_OK);
            lifecycle.finishReceive();
            CHECK(target.DragLeave() == S_OK);
            CHECK(lifecycle.leaves == 0);
            lifecycle.arm();
            const auto hovers = session.receiveHovers;
            effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
            CHECK(target.DragOver(0, {5, 5}, &effect) == S_OK);
            CHECK(effect == DROPEFFECT_NONE);
            CHECK(session.receiveHovers == hovers);
        }

        TEST_CASE("an armed receiver ignores hover, leave, and drop that arrive without a DragEnter")
        {
            OleApartment ole;
            REQUIRE(ole.initialized());
            TemporaryFile file;
            const std::vector<std::wstring> paths{file.path().wstring()};
            auto data = adapters::shell::makeDataObject(paths);
            REQUIRE(data.has_value());
            DropSession session;
            session.receiveOutcome = {core::Effect::Copy, false};
            Menu menu;
            ReceiveLifecycle lifecycle;
            lifecycle.arm();
            DropTarget target{session, dropData(), menu, lifecycle};
            DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;

            CHECK(target.DragOver(0, {5, 5}, &effect) == S_OK);
            CHECK(effect == DROPEFFECT_NONE);
            CHECK(session.receiveHovers == 0);
            // A DragLeave in receive mode always reaches the lifecycle (it belongs to the only live drag); the
            // Entered-to-Armed transition is a no-op while still Armed.
            CHECK(target.DragLeave() == S_OK);
            CHECK(lifecycle.leaves == 1);
            CHECK(lifecycle.stage == ReceiveLifecycle::Stage::Armed);
            effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
            CHECK(target.Drop(data->data.Get(), 0, {5, 5}, &effect) == S_OK);
            CHECK(effect == DROPEFFECT_NONE);
            CHECK(session.receiveDrops == 0);
            CHECK(lifecycle.finishes == 0);
            CHECK(lifecycle.stage == ReceiveLifecycle::Stage::Armed);
            CHECK(target.DragOver(0, {5, 5}, nullptr) == E_INVALIDARG);
            CHECK(target.Drop(data->data.Get(), 0, {5, 5}, nullptr) == E_INVALIDARG);
            CHECK(lifecycle.stage == ReceiveLifecycle::Stage::Armed);

            effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
            REQUIRE(target.DragEnter(data->data.Get(), 0, {5, 5}, &effect) == S_OK);
            CHECK(target.Drop(data->data.Get(), 0, {5, 5}, &effect) == S_OK);
            CHECK(effect == DROPEFFECT_COPY);
            CHECK(session.receiveDrops == 1);
        }
    }

} // namespace burlak::drag
