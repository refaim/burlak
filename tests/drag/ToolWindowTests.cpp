#include "drag/ToolWindow.hpp"

#include "adapters/shell/Shell.hpp"
#include "core/Policies.hpp"

#include <doctest/doctest.h>

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <new>
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

        class Screen final : public core::IScreen
        {
          public:
            std::optional<core::HostWindow> host;
            std::optional<core::Point> point;
            core::NativeWindow root{};
            core::NativeWindow console{11};
            bool left{};
            bool right{};

            [[nodiscard]] std::optional<core::Point> cursor() override
            {
                return point;
            }
            [[nodiscard]] bool buttonDown(core::Button button) override
            {
                return button == core::Button::Left ? left : right;
            }
            [[nodiscard]] core::NativeWindow windowAt(core::Point) override
            {
                return root;
            }
            [[nodiscard]] core::NativeWindow consoleWindow() override
            {
                return console;
            }
            [[nodiscard]] core::NativeWindow hostWindowHandle() override
            {
                return 0;
            }
            [[nodiscard]] std::optional<core::HostWindow> hostWindow() override
            {
                return host;
            }
            [[nodiscard]] std::optional<core::HostWindow> hostWindowAt(core::Point) override
            {
                return host;
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

        class Shell final : public core::IShell
        {
          public:
            class Data final : public DragData
            {
              public:
                [[nodiscard]] std::uintptr_t nativeHandle() const override
                {
                    return 1;
                }
            };

            bool prepares{true};
            bool throwAllocation{};
            std::size_t parsedPaths{1};
            int dragCalls{};
            bool allowedLink{};
            std::uintptr_t sourceHandle{};
            core::DragLoopOutcome dragOutcome{};
            std::optional<core::Effect> preferredEffect;
            std::function<void(core::NativeWindow)> duringDrag;

            [[nodiscard]] std::expected<PreparedDrag, core::Error> makeDataObject(
                std::span<const std::wstring>, std::optional<core::Effect> preferred) override
            {
                preferredEffect = preferred;
                if (throwAllocation) {
                    throw std::bad_alloc{};
                }
                if (!prepares) {
                    return std::unexpected(core::Error::NoSelection);
                }
                return PreparedDrag{.data = std::make_unique<Data>(), .parsedPaths = parsedPaths};
            }
            [[nodiscard]] core::DragLoopOutcome runDrag(core::NativeWindow owner, DragData &, std::uintptr_t source,
                                                        bool allowLink) override
            {
                ++dragCalls;
                allowedLink = allowLink;
                sourceHandle = source;
                if (duringDrag) {
                    duringDrag(owner);
                }
                return dragOutcome;
            }
            [[nodiscard]] std::expected<void, core::Error> copy(std::span<const std::wstring>, std::wstring_view,
                                                                core::Effect, core::NativeWindow) override
            {
                return {};
            }
        };

        class Input final : public core::IInput
        {
          public:
            std::vector<core::Button> presses;

            void release(core::Button) override
            {
            }
            void press(core::Button button) override
            {
                presses.push_back(button);
            }
            [[nodiscard]] core::ReplayOutcome replay(std::span<const core::MouseEvent>) override
            {
                return {true, 0, 0};
            }
        };

        class DropSession final : public core::IDropSession
        {
          public:
            std::optional<core::DropContext> sourceContext;
            std::optional<core::ReceiveSnapshot> receiveContext;
            std::vector<core::Point> snapshotRequests;
            int cancellations{};
            int sourceDrops{};
            int receiveDrops{};
            int refreshRequests{};
            bool acceptRequest{true};
            bool acceptRefresh{true};
            std::function<void()> duringRefreshRequest;
            std::function<void()> duringReceiveDrop;

            int sourceEnds{};

            void prepare(core::DropContext context) override
            {
                sourceContext = std::move(context);
            }
            void endSource() override
            {
                ++sourceEnds;
            }
            [[nodiscard]] core::Effect effect(core::Point, bool) const override
            {
                return core::Effect::Copy;
            }
            [[nodiscard]] core::Effect drop(core::Point, bool shift) override
            {
                ++sourceDrops;
                return shift ? core::Effect::Move : core::Effect::Copy;
            }
            [[nodiscard]] bool requestReceiveSnapshot(core::Point point) override
            {
                snapshotRequests.push_back(point);
                return acceptRequest;
            }
            [[nodiscard]] bool requestReceiveRefresh(core::Point) override
            {
                ++refreshRequests;
                if (duringRefreshRequest) {
                    duringRefreshRequest();
                }
                return acceptRefresh;
            }
            void prepareReceive(core::ReceiveSnapshot snapshot) override
            {
                receiveContext = std::move(snapshot);
            }
            void cancelReceive() override
            {
                ++cancellations;
                receiveContext.reset();
            }
            [[nodiscard]] core::Effect receiveEffect(core::Point, bool shift,
                                                     core::AllowedEffects allowed) const override
            {
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
                return 10;
            }
            [[nodiscard]] core::ReceiveDropOutcome receiveDrop(std::span<const std::wstring>, core::Point,
                                                               core::Effect effect) override
            {
                ++receiveDrops;
                if (duringReceiveDrop) {
                    duringReceiveDrop();
                }
                return core::receiveDropOutcome(effect, true);
            }
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
                        (L"burlak-tool-window-" + std::to_wstring(GetCurrentProcessId()))},
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
            [[nodiscard]] const std::filesystem::path &path() const
            {
                return path_;
            }

          private:
            std::filesystem::path root_;
            std::filesystem::path path_;
        };

        class Extraction final : public core::IExtraction
        {
          public:
            int calls{};
            int cleanups{};
            int retains{};
            bool succeeds{true};
            std::function<void()> duringRetain;

            [[nodiscard]] bool extract() override
            {
                ++calls;
                return succeeds;
            }
            void cleanup() override
            {
                ++cleanups;
            }
            void retain() override
            {
                if (duringRetain) {
                    duringRetain();
                }
                ++retains;
            }
        };

        class Files final : public core::IFiles
        {
          public:
            int sweeps{};
            bool alive{true};

            [[nodiscard]] std::expected<std::wstring, core::Error> runDirectory() override
            {
                return std::unexpected(core::Error::Unavailable);
            }
            [[nodiscard]] std::expected<std::wstring, core::Error> placeholder(std::wstring_view, bool) override
            {
                return std::unexpected(core::Error::Unavailable);
            }
            [[nodiscard]] bool nameBefore(std::wstring_view, std::wstring_view) const override
            {
                return false;
            }
            [[nodiscard]] std::expected<void, core::Error> removeTree(std::wstring_view) override
            {
                return {};
            }
            [[nodiscard]] std::expected<void, core::Error> touch(std::wstring_view) override
            {
                return {};
            }
            [[nodiscard]] bool processAlive(std::uint32_t) const override
            {
                return alive;
            }
            void sweep() override
            {
                ++sweeps;
            }
        };

        class Properties final : public core::IWindowProperties
        {
          public:
            std::optional<std::uint32_t> receiver;
            std::uint32_t process{7};

            void set(core::NativeWindow, std::uint32_t) override
            {
            }
            [[nodiscard]] std::optional<std::uint32_t> value(core::NativeWindow) const override
            {
                return receiver;
            }
            void remove(core::NativeWindow) override
            {
            }
            [[nodiscard]] std::uint32_t processId() const override
            {
                return process;
            }
        };

        class Menu final : public core::IDropMenu
        {
          public:
            [[nodiscard]] core::DropMenuChoice choose(core::NativeWindow, core::Point, core::AllowedEffects) override
            {
                return core::DropMenuChoice::Cancel;
            }
        };

        core::DropContext dropContext()
        {
            return {.press = {5, 5},
                    .source = core::PanelSide::Active,
                    .panels = {},
                    .host = std::nullopt,
                    .geometry = std::nullopt,
                    .panelsWindow = false,
                    .sourcePaths = {},
                    .destinationDirectory = std::nullopt};
        }

        core::ReceiveSnapshot receiveSnapshot()
        {
            return {.panelsWindow = true,
                    .panels = {core::PanelInfo{
                                   .visible = true, .realNames = true, .filePanel = true, .rect = {0, 0, 39, 24}},
                               std::nullopt},
                    .directories = {L"C:\\target", std::nullopt},
                    .host = core::HostWindow{10, {100, 120, 740, 520}, false},
                    .geometry = core::CellGeometry{{100, 120}, 8, 16}};
        }

        HANDLE WINAPI failCreateThread(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD)
        {
            return nullptr;
        }

        HANDLE WINAPI failCreateEvent(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCWSTR)
        {
            return nullptr;
        }

        void WINAPI shortSleep(DWORD)
        {
            Sleep(1);
        }

        HWND WINAPI failCreateWindow(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID)
        {
            return nullptr;
        }

        BOOL WINAPI reportInvisible(HWND)
        {
            return FALSE;
        }

        struct HeadlessWindowState
        {
            bool visible{};
            int placements{};
            core::PixelRect rectangle{};
            int captures{};
            int releases{};
            std::vector<std::pair<UINT_PTR, UINT>> timers;
        } headlessWindow;

        enum class JoinWaitResult : std::uint8_t
        {
            Success,
            WrongIndex,
            Failure,
        } joinWaitResult{};

        int joinWaitCalls{};
        std::chrono::steady_clock::time_point fakeTime{};

        std::chrono::steady_clock::time_point fakeNow()
        {
            return fakeTime;
        }

        HRESULT WINAPI joinThread(DWORD, DWORD, ULONG count, LPHANDLE handles, LPDWORD index)
        {
            ++joinWaitCalls;
            if (joinWaitResult == JoinWaitResult::Failure || count != 1) {
                return E_FAIL;
            }
            if (WaitForSingleObject(handles[0], 5000) != WAIT_OBJECT_0) {
                return E_FAIL;
            }
            *index = joinWaitResult == JoinWaitResult::WrongIndex ? 1U : 0U;
            return S_OK;
        }

        enum class RefreshWaitResult : std::uint8_t
        {
            Failure,
            WrongIndex
        } refreshWaitResult{};
        DWORD refreshWaitMilliseconds{};

        HRESULT WINAPI failRefreshWait(DWORD flags, DWORD milliseconds, ULONG count, LPHANDLE handles, LPDWORD index)
        {
            if (milliseconds == INFINITE) {
                return joinThread(flags, milliseconds, count, handles, index);
            }
            refreshWaitMilliseconds = milliseconds;
            if (refreshWaitResult == RefreshWaitResult::Failure) {
                return E_FAIL;
            }
            *index = 1;
            return S_OK;
        }

        BOOL WINAPI headlessSetWindowPos(HWND, HWND, int x, int y, int width, int height, UINT flags)
        {
            ++headlessWindow.placements;
            if ((flags & SWP_NOMOVE) == 0) {
                headlessWindow.rectangle = {x, y, x + width, y + height};
            }
            if ((flags & SWP_SHOWWINDOW) != 0) {
                headlessWindow.visible = true;
            }
            return TRUE;
        }

        BOOL WINAPI headlessIsWindowVisible(HWND)
        {
            return headlessWindow.visible ? TRUE : FALSE;
        }

        BOOL WINAPI headlessShowWindow(HWND, int command)
        {
            if (command == SW_HIDE) {
                headlessWindow.visible = false;
            }
            return TRUE;
        }

        HWND WINAPI headlessSetCapture(HWND window)
        {
            ++headlessWindow.captures;
            return window;
        }

        BOOL WINAPI headlessReleaseCapture()
        {
            ++headlessWindow.releases;
            return TRUE;
        }

        UINT_PTR WINAPI headlessSetTimer(HWND, UINT_PTR event, UINT duration, TIMERPROC)
        {
            headlessWindow.timers.emplace_back(event, duration);
            return event;
        }

        BOOL WINAPI headlessKillTimer(HWND, UINT_PTR)
        {
            return TRUE;
        }

        HWND WINAPI noPreviousWindow(HWND, UINT)
        {
            return nullptr;
        }

        HWND positionedToolWindow{};

        HWND WINAPI toolThenPreviousWindow(HWND window, UINT)
        {
            return window == reinterpret_cast<HWND>(10) ? positionedToolWindow : reinterpret_cast<HWND>(44);
        }

        struct DelayedThreadState
        {
            HANDLE gate{};
            LPTHREAD_START_ROUTINE entry{};
            LPVOID parameter{};
            DWORD id{};
            int sleeps{};
            int entries{};
            int joins{};
            int windows{};
        } delayedThread;

        DWORD WINAPI enterDelayedThread(LPVOID)
        {
            static_cast<void>(WaitForSingleObject(delayedThread.gate, INFINITE));
            ++delayedThread.entries;
            return delayedThread.entry(delayedThread.parameter);
        }

        HANDLE WINAPI createDelayedThread(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE entry, LPVOID parameter,
                                          DWORD, LPDWORD id)
        {
            delayedThread.entry = entry;
            delayedThread.parameter = parameter;
            const HANDLE thread = CreateThread(nullptr, 0, enterDelayedThread, nullptr, 0, id);
            delayedThread.id = *id;
            return thread;
        }

        void WINAPI countSleep(DWORD)
        {
            ++delayedThread.sleeps;
        }

        HWND WINAPI countFailedWindow(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE,
                                      LPVOID)
        {
            ++delayedThread.windows;
            return nullptr;
        }

        HRESULT WINAPI releaseDelayedThread(DWORD, DWORD, ULONG count, LPHANDLE handles, LPDWORD index)
        {
            ++delayedThread.joins;
            if (count != 1 || SetEvent(delayedThread.gate) == FALSE) {
                return E_FAIL;
            }
            for (int attempt = 0; attempt < 200; ++attempt) {
                if (PostThreadMessageW(delayedThread.id, WM_QUIT, 0, 0) != FALSE ||
                    WaitForSingleObject(handles[0], 0) == WAIT_OBJECT_0) {
                    break;
                }
                Sleep(1);
            }
            if (WaitForSingleObject(handles[0], 5000) != WAIT_OBJECT_0) {
                return E_FAIL;
            }
            *index = 0;
            return S_OK;
        }

        ToolWindowCalls headlessCalls()
        {
            auto calls = systemToolWindowCalls();
            calls.isWindowVisible = headlessIsWindowVisible;
            calls.setWindowPos = headlessSetWindowPos;
            calls.showWindow = headlessShowWindow;
            calls.setCapture = headlessSetCapture;
            calls.releaseCapture = headlessReleaseCapture;
            calls.setTimer = headlessSetTimer;
            calls.killTimer = headlessKillTimer;
            calls.getWindow = noPreviousWindow;
            calls.coWait = joinThread;
            return calls;
        }

        void resetHeadlessWindow()
        {
            headlessWindow = {};
            joinWaitResult = JoinWaitResult::Success;
            joinWaitCalls = 0;
            positionedToolWindow = nullptr;
        }

    } // namespace

    TEST_SUITE("tool window")
    {
        TEST_CASE("poll requests a receive snapshot, shows only panel pixels, and hides on button-up")
        {
            resetHeadlessWindow();
            Screen screen;
            screen.host = core::HostWindow{10, {100, 120, 740, 520}, false};
            screen.point = core::Point{20, 30};
            screen.root = 30;
            screen.left = true;
            Input input;
            Shell shell;
            DropSession session;
            Extraction extraction;
            Files files;
            Properties properties;
            Menu menu;
            const auto calls = headlessCalls();
            ToolWindow tool{screen, input, shell, dropData(), session, extraction, files, properties, menu, calls};
            REQUIRE(tool.start());
            session.duringRefreshRequest = [&] { tool.completeReceiveRefresh(true); };
            const auto window = reinterpret_cast<HWND>(tool.nativeWindow());
            REQUIRE(window != nullptr);
            REQUIRE(headlessWindow.timers.size() == 2);
            CHECK(headlessWindow.timers[0] == std::pair<UINT_PTR, UINT>{externalDragPollTimerId(), 50});
            CHECK(headlessWindow.timers[1] == std::pair<UINT_PTR, UINT>{extractionSweepTimerId(), 60000});

            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            CHECK(session.snapshotRequests.empty());
            screen.point = core::Point{108, 200};
            screen.root = 10;
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            CHECK(session.snapshotRequests == std::vector<core::Point>{{108, 200}});
            CHECK(tool.active());
            CHECK_FALSE(headlessWindow.visible);

            tool.receiveSnapshot(receiveSnapshot());
            CHECK(session.receiveContext.has_value());
            CHECK(headlessWindow.rectangle == core::PixelRect{100, 120, 420, 520});
            CHECK(headlessWindow.visible);
            CHECK(headlessWindow.captures == 0);

            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            CHECK(headlessWindow.visible);
            screen.left = false;
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            CHECK_FALSE(headlessWindow.visible);
            CHECK_FALSE(tool.active());
            CHECK(session.cancellations == 1);
            tool.stop();
        }

        TEST_CASE("poll rejects live competing panes but accepts a stale focused-Far property")
        {
            resetHeadlessWindow();
            Screen screen;
            screen.host = core::HostWindow{10, {100, 120, 740, 520}, false};
            screen.point = core::Point{20, 30};
            screen.root = 30;
            screen.right = true;
            Input input;
            Shell shell;
            DropSession session;
            Extraction extraction;
            Files files;
            Properties properties;
            properties.receiver = 8;
            Menu menu;
            const auto calls = headlessCalls();
            ToolWindow tool{screen, input, shell, dropData(), session, extraction, files, properties, menu, calls};
            REQUIRE(tool.start());
            const auto window = reinterpret_cast<HWND>(tool.nativeWindow());
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            screen.point = core::Point{108, 200};
            screen.root = 40;
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            CHECK(session.snapshotRequests.empty());
            screen.root = 10;
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            CHECK(session.snapshotRequests.empty());

            files.alive = false;
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            REQUIRE(session.snapshotRequests.size() == 1);
            auto invalid = receiveSnapshot();
            invalid.panelsWindow = false;
            tool.receiveSnapshot(invalid);
            CHECK_FALSE(headlessWindow.visible);
            CHECK(tool.active());
            screen.right = false;
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            CHECK_FALSE(tool.active());
            tool.stop();
        }

        TEST_CASE("entered receive survives button-up until Drop or DragLeave")
        {
            resetHeadlessWindow();
            Screen screen;
            screen.host = core::HostWindow{10, {100, 120, 740, 520}, false};
            screen.point = core::Point{20, 30};
            screen.root = 30;
            screen.left = true;
            Input input;
            Shell shell;
            DropSession session;
            Extraction extraction;
            Files files;
            Properties properties;
            Menu menu;
            const auto calls = headlessCalls();
            ToolWindow tool{screen, input, shell, dropData(), session, extraction, files, properties, menu, calls};
            REQUIRE(tool.start());
            const auto window = reinterpret_cast<HWND>(tool.nativeWindow());
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            screen.point = core::Point{108, 200};
            screen.root = 10;
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            tool.receiveSnapshot(receiveSnapshot());
            REQUIRE(headlessWindow.visible);

            CHECK(tool.dragEnter(0, 0, {108, 200}, DROPEFFECT_COPY | DROPEFFECT_MOVE) == DROPEFFECT_NONE);
            screen.left = false;
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            CHECK(headlessWindow.visible);
            CHECK(tool.active());
            tool.dragLeave();
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            CHECK_FALSE(headlessWindow.visible);
            CHECK_FALSE(tool.active());
            tool.stop();
        }

        TEST_CASE("abort clears an entered or dropping receiver and the own pid is its own receiver")
        {
            OleApartment ole;
            REQUIRE(ole.initialized());
            TemporaryFile file;
            const std::vector<std::wstring> paths{file.path().wstring()};
            auto data = adapters::shell::makeDataObject(paths);
            REQUIRE(data.has_value());
            resetHeadlessWindow();
            Screen screen;
            screen.host = core::HostWindow{10, {100, 120, 740, 520}, false};
            screen.point = core::Point{20, 30};
            screen.root = 30;
            screen.left = true;
            Input input;
            Shell shell;
            DropSession session;
            Extraction extraction;
            Files files;
            Properties properties;
            properties.receiver = properties.process; // the last-focused Far is this process
            Menu menu;
            const auto calls = headlessCalls();
            ToolWindow tool{screen, input, shell, dropData(), session, extraction, files, properties, menu, calls};
            REQUIRE(tool.start());
            const auto window = reinterpret_cast<HWND>(tool.nativeWindow());
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            screen.point = core::Point{108, 200};
            screen.root = 10;
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            REQUIRE(session.snapshotRequests == std::vector<core::Point>{{108, 200}});
            tool.receiveSnapshot(receiveSnapshot());
            const auto nativeData = reinterpret_cast<std::uintptr_t>(data->data.Get());

            SUBCASE("abort while entered")
            {
                REQUIRE(tool.dragEnter(nativeData, 0, {108, 200}, DROPEFFECT_COPY) == DROPEFFECT_COPY);
                tool.abort();
                CHECK_FALSE(tool.active());
                CHECK_FALSE(headlessWindow.visible);
            }
            SUBCASE("abort while dropping")
            {
                REQUIRE(tool.dragEnter(nativeData, 0, {108, 200}, DROPEFFECT_COPY) == DROPEFFECT_COPY);
                session.duringRefreshRequest = [&] { tool.completeReceiveRefresh(true); };
                session.duringReceiveDrop = [&] { tool.abort(); };
                static_cast<void>(tool.drop(nativeData, 0, {108, 200}, DROPEFFECT_COPY));
                CHECK_FALSE(tool.active());
                CHECK_FALSE(headlessWindow.visible);
            }
            tool.stop();
        }

        TEST_CASE("entered receive times out two minutes after button-up, and a later Drop is refused")
        {
            OleApartment ole;
            REQUIRE(ole.initialized());
            TemporaryFile file;
            const std::vector<std::wstring> paths{file.path().wstring()};
            auto data = adapters::shell::makeDataObject(paths);
            REQUIRE(data.has_value());
            resetHeadlessWindow();
            fakeTime = {};
            Screen screen;
            screen.host = core::HostWindow{10, {100, 120, 740, 520}, false};
            screen.point = core::Point{20, 30};
            screen.root = 30;
            screen.left = true;
            Input input;
            Shell shell;
            DropSession session;
            Extraction extraction;
            Files files;
            Properties properties;
            Menu menu;
            auto calls = headlessCalls();
            calls.now = fakeNow;
            ToolWindow tool{screen, input, shell, dropData(), session, extraction, files, properties, menu, calls};
            REQUIRE(tool.start());
            const auto window = reinterpret_cast<HWND>(tool.nativeWindow());
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            screen.point = core::Point{108, 200};
            screen.root = 10;
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            tool.receiveSnapshot(receiveSnapshot());
            const auto nativeData = reinterpret_cast<std::uintptr_t>(data->data.Get());
            static_cast<void>(tool.dragEnter(nativeData, 0, {108, 200}, DROPEFFECT_COPY));
            screen.left = false;
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            // Ten seconds (the old ceiling) no longer tears the overlay down: a large-archive extraction may still
            // be running in the source's QueryContinueDrag.
            fakeTime += std::chrono::seconds{10};
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            CHECK(headlessWindow.visible);
            fakeTime += core::receiveEnteredTimeout - std::chrono::seconds{10} - std::chrono::milliseconds{1};
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            CHECK(headlessWindow.visible);
            fakeTime += std::chrono::milliseconds{1};
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            CHECK_FALSE(headlessWindow.visible);
            CHECK_FALSE(tool.active());

            // A source that finally sends Drop after the timeout finds an idle overlay: it must be answered NONE
            // with no shell copy and no replay (OLE does not re-hit-test after DRAGDROP_S_DROP).
            CHECK(tool.drop(nativeData, 0, {108, 200}, DROPEFFECT_COPY) == DROPEFFECT_NONE);
            CHECK(session.receiveDrops == 0);
            CHECK(session.sourceDrops == 0);
            tool.stop();
        }

        TEST_CASE("dropping ignores poll and sweep timers and rejects a second DragEnter")
        {
            OleApartment ole;
            REQUIRE(ole.initialized());
            TemporaryFile file;
            const std::vector<std::wstring> paths{file.path().wstring()};
            auto data = adapters::shell::makeDataObject(paths);
            REQUIRE(data.has_value());
            resetHeadlessWindow();
            Screen screen;
            screen.host = core::HostWindow{10, {100, 120, 740, 520}, false};
            screen.point = core::Point{20, 30};
            screen.root = 30;
            screen.left = true;
            Input input;
            Shell shell;
            DropSession session;
            Extraction extraction;
            Files files;
            Properties properties;
            Menu menu;
            const auto calls = headlessCalls();
            ToolWindow tool{screen, input, shell, dropData(), session, extraction, files, properties, menu, calls};
            REQUIRE(tool.start());
            const auto window = reinterpret_cast<HWND>(tool.nativeWindow());
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            screen.point = core::Point{108, 200};
            screen.root = 10;
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            tool.receiveSnapshot(receiveSnapshot());
            const auto nativeData = reinterpret_cast<std::uintptr_t>(data->data.Get());
            REQUIRE(tool.dragEnter(nativeData, 0, {108, 200}, DROPEFFECT_COPY) == DROPEFFECT_COPY);
            screen.left = false;
            session.duringRefreshRequest = [&] { tool.completeReceiveRefresh(true); };
            session.duringReceiveDrop = [&] {
                SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
                SendMessageW(window, WM_TIMER, extractionSweepTimerId(), 0);
                CHECK(headlessWindow.visible);
                CHECK(files.sweeps == 0);
                CHECK(tool.dragEnter(nativeData, 0, {108, 200}, DROPEFFECT_COPY) == DROPEFFECT_NONE);
            };
            CHECK(tool.drop(nativeData, 0, {108, 200}, DROPEFFECT_COPY) == DROPEFFECT_COPY);
            CHECK_FALSE(headlessWindow.visible);
            CHECK_FALSE(tool.active());
            CHECK(files.sweeps == 0);
            tool.stop();
        }

        TEST_CASE("receive refresh rejection and both bounded-wait failures cancel before the shell operation")
        {
            OleApartment ole;
            REQUIRE(ole.initialized());
            TemporaryFile file;
            const std::vector<std::wstring> paths{file.path().wstring()};
            auto data = adapters::shell::makeDataObject(paths);
            REQUIRE(data.has_value());

            for (int scenario = 0; scenario < 4; ++scenario) {
                resetHeadlessWindow();
                refreshWaitMilliseconds = 0;
                Screen screen;
                screen.host = core::HostWindow{10, {100, 120, 740, 520}, false};
                screen.point = core::Point{20, 30};
                screen.root = 30;
                screen.left = true;
                Input input;
                Shell shell;
                DropSession session;
                session.acceptRefresh = scenario != 0;
                Extraction extraction;
                Files files;
                Properties properties;
                Menu menu;
                auto calls = headlessCalls();
                if (scenario != 3) {
                    calls.coWait = failRefreshWait;
                }
                refreshWaitResult = scenario == 1 ? RefreshWaitResult::Failure : RefreshWaitResult::WrongIndex;
                ToolWindow tool{screen, input, shell, dropData(), session, extraction, files, properties, menu, calls};
                REQUIRE(tool.start());
                if (scenario == 3) {
                    session.duringRefreshRequest = [&] { tool.completeReceiveRefresh(false); };
                }
                const auto window = reinterpret_cast<HWND>(tool.nativeWindow());
                SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
                screen.point = core::Point{108, 200};
                screen.root = 10;
                SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
                tool.receiveSnapshot(receiveSnapshot());
                const auto nativeData = reinterpret_cast<std::uintptr_t>(data->data.Get());
                REQUIRE(tool.dragEnter(nativeData, 0, {108, 200}, DROPEFFECT_COPY) == DROPEFFECT_COPY);

                CHECK(tool.drop(nativeData, 0, {108, 200}, DROPEFFECT_COPY) == DROPEFFECT_NONE);
                CHECK(session.refreshRequests == 1);
                CHECK(session.receiveDrops == 0);
                CHECK(refreshWaitMilliseconds == (scenario == 1 || scenario == 2 ? 10000U : 0U));
                CHECK_FALSE(tool.active());
                tool.stop();
            }
        }

        TEST_CASE("poll tolerates incomplete samples and rejected or cancelled snapshot requests")
        {
            resetHeadlessWindow();
            Screen screen;
            Input input;
            Shell shell;
            DropSession session;
            Extraction extraction;
            Files files;
            Properties properties;
            Menu menu;
            const auto calls = headlessCalls();
            ToolWindow tool{screen, input, shell, dropData(), session, extraction, files, properties, menu, calls};
            REQUIRE(tool.start());
            const auto window = reinterpret_cast<HWND>(tool.nativeWindow());

            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            screen.left = true;
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            screen.point = core::Point{20, 30};
            screen.root = 30;
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            screen.point.reset();
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            screen.point = core::Point{108, 200};
            screen.host.reset();
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            screen.host = core::HostWindow{10, {100, 120, 740, 520}, false};
            screen.root = 10;
            session.acceptRequest = false;
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            CHECK(tool.active());
            tool.abort();
            CHECK_FALSE(tool.active());

            screen.left = false;
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            screen.left = true;
            screen.point = core::Point{108, 200};
            screen.root = 10;
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            screen.left = false;
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            screen.left = true;
            screen.point = core::Point{20, 30};
            screen.root = 30;
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            screen.point = core::Point{108, 200};
            screen.root = 10;
            session.acceptRequest = true;
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            CHECK(tool.active());
            tool.abort();
            CHECK_FALSE(tool.active());
            tool.receiveSnapshot(receiveSnapshot());
            CHECK_FALSE(tool.active());

            screen.left = false;
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            screen.left = true;
            screen.point = core::Point{20, 30};
            screen.root = 30;
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            screen.point = core::Point{108, 200};
            screen.root = 10;
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            tool.receiveSnapshot(receiveSnapshot());
            REQUIRE(tool.active());
            tool.abort();
            CHECK_FALSE(tool.active());
            tool.stop();
        }

        TEST_CASE("a receive snapshot that cannot make the overlay visible is rejected")
        {
            resetHeadlessWindow();
            Screen screen;
            screen.host = core::HostWindow{10, {100, 120, 740, 520}, false};
            screen.point = core::Point{20, 30};
            screen.root = 30;
            screen.left = true;
            Input input;
            Shell shell;
            DropSession session;
            Extraction extraction;
            Files files;
            Properties properties;
            Menu menu;
            auto calls = headlessCalls();
            calls.isWindowVisible = reportInvisible;
            ToolWindow tool{screen, input, shell, dropData(), session, extraction, files, properties, menu, calls};
            REQUIRE(tool.start());
            const auto window = reinterpret_cast<HWND>(tool.nativeWindow());
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            screen.point = core::Point{108, 200};
            screen.root = 10;
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            tool.receiveSnapshot(receiveSnapshot());
            CHECK(tool.active());
            CHECK(session.cancellations == 1);
            screen.left = false;
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            CHECK_FALSE(tool.active());
            tool.stop();
        }

        TEST_CASE("the minute timer sweeps only while the single window state machine is idle")
        {
            resetHeadlessWindow();
            Screen screen;
            screen.host = core::HostWindow{10, {100, 120, 420, 360}, false};
            Input input;
            Shell shell;
            DropSession session;
            Extraction extraction;
            Files files;
            Properties properties;
            Menu menu;
            auto calls = headlessCalls();
            calls.getWindow = toolThenPreviousWindow;
            ToolWindow tool{screen, input, shell, dropData(), session, extraction, files, properties, menu, calls};
            REQUIRE(tool.start());
            const auto window = reinterpret_cast<HWND>(tool.nativeWindow());
            positionedToolWindow = window;

            SendMessageW(window, WM_TIMER, extractionSweepTimerId(), 0);
            CHECK(files.sweeps == 1);
            const std::vector<std::wstring> paths{L"C:\\one.txt"};
            REQUIRE(tool.prepare(paths, core::Button::Left, false, dropContext()));
            CHECK(tool.active());
            SendMessageW(window, WM_TIMER, extractionSweepTimerId(), 0);
            CHECK(files.sweeps == 1);
            tool.abort();
            CHECK_FALSE(tool.active());

            screen.point = core::Point{10, 10};
            screen.root = 30;
            screen.left = true;
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            screen.root = 10;
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            REQUIRE(tool.active());
            SendMessageW(window, WM_TIMER, extractionSweepTimerId(), 0);
            CHECK(files.sweeps == 1);
            screen.left = false;
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            SendMessageW(window, WM_TIMER, extractionSweepTimerId(), 0);
            CHECK(files.sweeps == 2);
            tool.stop();
        }

        TEST_CASE("source mode still runs OLE, replays own drops, and retains extraction before becoming idle")
        {
            resetHeadlessWindow();
            Screen screen;
            screen.host = core::HostWindow{10, {100, 120, 420, 360}, false};
            screen.point = core::Point{200, 200};
            screen.root = 40;
            Input input;
            Shell shell;
            shell.dragOutcome = {DRAGDROP_S_DROP, DROPEFFECT_COPY};
            DropSession session;
            Extraction extraction;
            Files files;
            Properties properties;
            Menu menu;
            auto calls = headlessCalls();
            calls.getWindow = toolThenPreviousWindow;
            ToolWindow tool{screen, input, shell, dropData(), session, extraction, files, properties, menu, calls};
            REQUIRE(tool.start());
            const auto window = reinterpret_cast<HWND>(tool.nativeWindow());
            positionedToolWindow = window;
            const std::vector<std::wstring> paths{L"C:\\one.txt"};
            REQUIRE(tool.prepare(paths, core::Button::Right, true, dropContext()));
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            CHECK(shell.preferredEffect == core::Effect::Copy);
            CHECK(tool.hasData());
            REQUIRE(tool.showAndArm());
            SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            CHECK(input.presses == std::vector<core::Button>{core::Button::Right});
            CHECK(headlessWindow.captures == 1);
            shell.duringDrag = [&](core::NativeWindow owner) {
                CHECK(tool.active());
                auto &source = *reinterpret_cast<IDropSource *>(shell.sourceHandle);
                CHECK(source.GiveFeedback(DROPEFFECT_COPY) == DRAGDROP_S_USEDEFAULTCURSORS);
                CHECK(source.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_DROP);
                CHECK(owner == tool.nativeWindow());
                SendMessageW(window, WM_TIMER, externalDragPollTimerId(), 0);
            };
            bool idleDuringRetain{};
            extraction.duringRetain = [&] { idleDuringRetain = !tool.active(); };
            SendMessageW(window, WM_RBUTTONDOWN, 0, 0);
            CHECK(shell.dragCalls == 1);
            CHECK_FALSE(shell.allowedLink);
            CHECK(extraction.calls == 1);
            CHECK(extraction.retains == 1);
            CHECK_FALSE(idleDuringRetain);
            CHECK_FALSE(tool.active());
            CHECK_FALSE(tool.hasData());
            CHECK(headlessWindow.releases == 1);

            // The same-Far replay runs only from inside the OLE drag loop; a Drop that reaches the overlay in any
            // other window state is answered NONE (finding: a torn-down overlay must not replay a stale selection).
            REQUIRE(tool.prepare(paths, core::Button::Left, false, dropContext()));
            REQUIRE(tool.showAndArm());
            shell.duringDrag = [&](core::NativeWindow) { tool.drop({45, 6}, true); };
            SendMessageW(window, WM_LBUTTONDOWN, 0, 0);
            CHECK(session.sourceDrops == 1);
            CHECK(session.sourceEnds >= 1);

            // Outside the drag loop the overlay is inert: the same-Far replay never reaches the session.
            REQUIRE(tool.prepare(paths, core::Button::Left, false, dropContext()));
            tool.drop({45, 6}, true);
            CHECK(session.sourceDrops == 1);
            tool.abort();
            tool.stop();
        }

        TEST_CASE("arm timeout, incomplete payloads, and invisible placement clean source state")
        {
            resetHeadlessWindow();
            Screen screen;
            screen.host = core::HostWindow{10, {100, 120, 420, 360}, true};
            Input input;
            Shell shell;
            DropSession session;
            Extraction extraction;
            Files files;
            Properties properties;
            Menu menu;
            auto calls = headlessCalls();
            ToolWindow tool{screen, input, shell, dropData(), session, extraction, files, properties, menu, calls};
            REQUIRE(tool.start());
            const auto window = reinterpret_cast<HWND>(tool.nativeWindow());
            const std::vector<std::wstring> paths{L"C:\\one.txt", L"C:\\two.txt"};
            SendMessageW(window, WM_TIMER, armTimerId(), 0);
            SendMessageW(window, WM_TIMER, armTimerId() + 99, 0);
            CHECK_FALSE(tool.showAndArm());
            SendMessageW(window, WM_LBUTTONDOWN, 0, 0);
            shell.prepares = false;
            CHECK_FALSE(tool.prepare(paths, core::Button::Left, true, dropContext()));
            shell.prepares = true;
            shell.parsedPaths = 1;
            CHECK_FALSE(tool.prepare(paths, core::Button::Left, true, dropContext()));
            CHECK_FALSE(tool.hasData());

            shell.parsedPaths = 2;
            REQUIRE(tool.prepare(paths, core::Button::Left, true, dropContext()));
            CHECK_FALSE(tool.prepare(paths, core::Button::Left, true, dropContext()));
            SendMessageW(window, WM_LBUTTONDOWN, 0, 0);
            REQUIRE(tool.showAndArm());
            CHECK_FALSE(tool.showAndArm());
            SendMessageW(window, WM_TIMER, armTimerId(), 0);
            REQUIRE(tool.prepare(paths, core::Button::Left, true, dropContext()));
            REQUIRE(tool.showAndArm());
            SendMessageW(window, WM_TIMER, armTimerId(), 0);
            CHECK_FALSE(tool.active());
            CHECK(extraction.cleanups == 2);

            calls = headlessCalls();
            calls.isWindowVisible = reportInvisible;
            ToolWindow invisible{screen, input, shell, dropData(), session, extraction, files, properties, menu, calls};
            REQUIRE(invisible.start());
            REQUIRE(invisible.prepare(paths, core::Button::Left, true, dropContext()));
            CHECK_FALSE(invisible.showAndArm());
            CHECK(extraction.cleanups == 3);

            screen.host.reset();
            ToolWindow noHost{screen,     input, shell,      dropData(), session,
                              extraction, files, properties, menu,       headlessCalls()};
            REQUIRE(noHost.start());
            REQUIRE(noHost.prepare(paths, core::Button::Left, true, dropContext()));
            CHECK_FALSE(noHost.showAndArm());
            noHost.stop();
            invisible.stop();
            tool.stop();
        }

        TEST_CASE("private messages require zero parameters and an in-process guarded slot")
        {
            resetHeadlessWindow();
            Screen screen;
            screen.host = core::HostWindow{10, {100, 120, 420, 360}, false};
            Input input;
            Shell shell;
            DropSession session;
            Extraction extraction;
            Files files;
            Properties properties;
            Menu menu;
            const auto calls = headlessCalls();
            ToolWindow tool{screen, input, shell, dropData(), session, extraction, files, properties, menu, calls};
            REQUIRE(tool.start());
            const auto window = reinterpret_cast<HWND>(tool.nativeWindow());

            for (const UINT message :
                 {WM_USER + 0x101, WM_USER + 0x102, WM_USER + 0x103, WM_USER + 0x104, WM_USER + 0x105}) {
                CHECK(SendMessageW(window, message, 1, 0) == 0);
                CHECK(SendMessageW(window, message, 0, 1) == 0);
                CHECK(SendMessageW(window, message, 0, 0) == 0);
            }
            CHECK_FALSE(tool.hasData());
            shell.throwAllocation = true;
            CHECK_FALSE(
                tool.prepare(std::vector<std::wstring>{L"C:\\one.txt"}, core::Button::Left, false, dropContext()));
            tool.stop();
        }

        TEST_CASE("thread, readiness event, native window, and join failures remain bounded")
        {
            Screen screen;
            Input input;
            Shell shell;
            DropSession session;
            Extraction extraction;
            Files files;
            Properties properties;
            Menu menu;

            auto calls = systemToolWindowCalls();
            calls.createEvent = failCreateEvent;
            ToolWindow noEvent{screen, input, shell, dropData(), session, extraction, files, properties, menu, calls};
            CHECK_FALSE(noEvent.start());

            calls = systemToolWindowCalls();
            calls.createThread = failCreateThread;
            ToolWindow noThread{screen, input, shell, dropData(), session, extraction, files, properties, menu, calls};
            CHECK_FALSE(noThread.start());

            calls = systemToolWindowCalls();
            calls.createWindow = failCreateWindow;
            calls.sleep = shortSleep;
            ToolWindow noWindow{screen, input, shell, dropData(), session, extraction, files, properties, menu, calls};
            CHECK_FALSE(noWindow.start());

            delayedThread = {};
            delayedThread.gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            REQUIRE(delayedThread.gate != nullptr);
            calls = systemToolWindowCalls();
            calls.createThread = createDelayedThread;
            calls.sleep = countSleep;
            calls.createWindow = countFailedWindow;
            calls.coWait = releaseDelayedThread;
            ToolWindow delayed{screen, input, shell, dropData(), session, extraction, files, properties, menu, calls};
            CHECK_FALSE(delayed.start());
            CHECK(delayedThread.sleeps == 200);
            CHECK(delayedThread.joins == 1);
            CHECK(delayedThread.entries == 1);
            CHECK(delayedThread.windows == 0);
            CloseHandle(delayedThread.gate);

            for (const auto result : {JoinWaitResult::Failure, JoinWaitResult::WrongIndex}) {
                resetHeadlessWindow();
                joinWaitResult = result;
                const auto headless = headlessCalls();
                ToolWindow joined{screen,     input, shell,      dropData(), session,
                                  extraction, files, properties, menu,       headless};
                REQUIRE(joined.start());
                // The class exists only while a tool runs; a window of it with no state pointer takes the
                // DefWindowProc path of the procedure.
                const HWND withoutState = CreateWindowExW(0, L"BurlakToolWindow", L"", WS_POPUP, 0, 0, 1, 1, nullptr,
                                                          nullptr, GetModuleHandleW(nullptr), nullptr);
                REQUIRE(withoutState != nullptr);
                DestroyWindow(withoutState);
                joined.stop();
                CHECK(joinWaitCalls == 1);
                CHECK(joined.nativeWindow() == 0);
            }
        }

        TEST_CASE("the window class belongs to this code's module while a tool runs and is gone afterwards")
        {
            Screen screen;
            Input input;
            Shell shell;
            DropSession session;
            Extraction extraction;
            Files files;
            Properties properties;
            Menu menu;
            // The module that holds the window procedure is the one holding every function of ToolWindow.cpp.
            HMODULE module{};
            REQUIRE(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCWSTR>(&armTimerId), &module) != FALSE);
            WNDCLASSEXW info{};
            info.cbSize = sizeof(info);

            resetHeadlessWindow();
            ToolWindow tool{screen,     input, shell,      dropData(), session,
                            extraction, files, properties, menu,       headlessCalls()};
            REQUIRE(tool.start());
            REQUIRE(GetClassInfoExW(module, L"BurlakToolWindow", &info) != FALSE);
            CHECK(info.hInstance == module);
            // A second tool in the same process finds the class registered by this very code and shares it.
            ToolWindow second{screen,     input, shell,      dropData(), session,
                              extraction, files, properties, menu,       headlessCalls()};
            REQUIRE(second.start());
            second.stop();
            CHECK(GetClassInfoExW(module, L"BurlakToolWindow", &info) != FALSE);
            tool.stop();
            // Unregistered with the last window: a plugin DLL may be unloaded, and a class left behind under the
            // host's module with a procedure in the unloaded image would make the next CreateWindowExW fast-fail.
            CHECK(GetClassInfoExW(module, L"BurlakToolWindow", &info) == FALSE);

            // A foreign class of the same name under this module is refused, not adopted or unregistered.
            WNDCLASSEXW foreign{};
            foreign.cbSize = sizeof(foreign);
            foreign.lpfnWndProc = DefWindowProcW;
            foreign.hInstance = module;
            foreign.lpszClassName = L"BurlakToolWindow";
            REQUIRE(RegisterClassExW(&foreign) != 0);
            auto quick = headlessCalls();
            quick.sleep = shortSleep;
            ToolWindow refused{screen, input, shell, dropData(), session, extraction, files, properties, menu, quick};
            CHECK_FALSE(refused.start());
            CHECK(refused.nativeWindow() == 0);
            REQUIRE(GetClassInfoExW(module, L"BurlakToolWindow", &info) != FALSE);
            CHECK(info.lpfnWndProc == DefWindowProcW);
            REQUIRE(UnregisterClassW(L"BurlakToolWindow", module) != FALSE);
        }
    }

} // namespace burlak::drag
