#include "adapters/shell/Shell.hpp"
#include "adapters/win/Input.hpp"
#include "drag/ExtractionWait.hpp"
#include "drag/ToolWindow.hpp"
#include "plugin/Composition.hpp"
#include "plugin/Firewall.hpp"

#include <doctest/doctest.h>

#include <plugin.hpp>

#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace
{

    std::atomic<int> synchros{};
    bool throwFromFar{};
    bool dispatchSynchro{};
    bool receivePanel{};
    bool sameFarPanels{};
    int panelUpdates{};
    int panelRedraws{};
    std::wstring receiveDirectory;
    std::vector<INPUT_RECORD> replayedRecords;

    class TemporaryDropFiles final
    {
      public:
        // The shell hands back canonical long names through CF_HDROP whatever spelling it was given, and the
        // runner's %TEMP% is an 8.3 short path: the root is canonicalised so the strings the test builds are the
        // strings the receiver is expected to copy.
        TemporaryDropFiles()
            : root_{std::filesystem::canonical(std::filesystem::temp_directory_path()) /
                    (L"burlak-plugin-drop-" + std::to_wstring(GetCurrentProcessId()))},
              paths_{root_ / L"one.txt", root_ / L"two.txt"}
        {
            std::error_code ignored;
            std::filesystem::remove_all(root_, ignored);
            std::filesystem::create_directories(root_);
            const std::ofstream first{paths_[0]};
            const std::ofstream second{paths_[1]};
        }

        ~TemporaryDropFiles()
        {
            std::error_code ignored;
            std::filesystem::remove_all(root_, ignored);
        }

        [[nodiscard]] std::vector<std::wstring> paths() const
        {
            return {paths_[0].wstring(), paths_[1].wstring()};
        }

        [[nodiscard]] static std::vector<std::wstring> canonical(std::span<const std::wstring> paths)
        {
            std::vector<std::wstring> result;
            for (const auto &path : paths) {
                std::error_code ignored;
                result.push_back(std::filesystem::canonical(path, ignored).wstring());
            }
            return result;
        }

      private:
        std::filesystem::path root_;
        std::array<std::filesystem::path, 2> paths_;
    };

    constexpr burlak::core::Point sameFarDropPoint{45, 7};

    void WINAPI replayMouseEvent(DWORD flags, DWORD, DWORD, DWORD, ULONG_PTR)
    {
        if (flags == (MOUSEEVENTF_MOVE | MOUSEEVENTF_LEFTDOWN)) {
            const auto tool = burlak::plugin::composition().toolWindow();
            static_cast<void>(SendMessageW(reinterpret_cast<HWND>(tool), WM_LBUTTONDOWN, 0, 0));
        }
    }

    BOOL WINAPI replayConsoleInfo(HANDLE, PCONSOLE_SCREEN_BUFFER_INFO info)
    {
        *info = {};
        info->dwSize = {80, 9001};
        info->srWindow = {0, 0, 79, 49};
        return TRUE;
    }

    BOOL WINAPI captureConsoleInput(HANDLE, const INPUT_RECORD *records, DWORD count, LPDWORD written)
    {
        replayedRecords.assign(records, records + count);
        *written = count;
        return TRUE;
    }

    class ExtractionSession final : public burlak::core::IExtractionSession
    {
      public:
        bool accepts{true};
        int requests{};
        int cleanups{};
        int retained{};

        [[nodiscard]] bool requestExtraction() override
        {
            ++requests;
            return accepts;
        }

        void cleanup() override
        {
            ++cleanups;
        }

        void retain() override
        {
            ++retained;
        }
    };

    burlak::drag::ExtractionWait *activeWait{};
    bool waitOutcome{true};
    HRESULT waitStatus{S_OK};
    DWORD waitIndex{};
    BOOL resetStatus{TRUE};
    std::function<void()> waitAction;

    HANDLE WINAPI createTestEvent(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCWSTR)
    {
        return CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    HANDLE WINAPI failCreateEvent(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCWSTR)
    {
        return nullptr;
    }

    BOOL WINAPI resetTestEvent(HANDLE event)
    {
        return resetStatus == FALSE ? FALSE : ResetEvent(event);
    }

    HRESULT WINAPI waitTestEvent(DWORD, DWORD, ULONG, LPHANDLE, LPDWORD index)
    {
        *index = waitIndex;
        if (waitStatus == S_OK) {
            if (auto action = std::exchange(waitAction, {})) {
                action();
            } else {
                activeWait->complete(waitOutcome);
            }
        }
        return waitStatus;
    }

    burlak::drag::ExtractionWaitCalls waitCalls()
    {
        return {createTestEvent, resetTestEvent, SetEvent, CloseHandle, waitTestEvent};
    }

    intptr_t WINAPI panelControl(HANDLE panel, FILE_CONTROL_COMMANDS command, intptr_t, void *parameter)
    {
        if (command == FCTL_GETPANELINFO) {
            auto &info = *static_cast<PanelInfo *>(parameter);
            info.Flags = sameFarPanels || panel == PANEL_ACTIVE ? PFLAGS_VISIBLE | PFLAGS_REALNAMES : PFLAGS_NONE;
            info.PanelType = PTYPE_FILEPANEL;
            info.PanelRect = panel == PANEL_ACTIVE ? RECT{0, 0, 39, 24} : RECT{40, 0, 79, 24};
            info.SelectedItemsNumber = sameFarPanels && panel == PANEL_ACTIVE ? 1 : 0;
            info.CurrentItem = 0;
            info.TopPanelItem = 0;
            return 1;
        }
        if (sameFarPanels && panel == PANEL_ACTIVE && command == FCTL_GETSELECTEDPANELITEM) {
            constexpr wchar_t itemName[] = L"source.txt";
            constexpr auto bytes = sizeof(PluginPanelItem) + sizeof(itemName);
            if (parameter == nullptr) {
                return bytes;
            }
            auto &request = *static_cast<FarGetPluginPanelItem *>(parameter);
            *request.Item = {};
            auto *name =
                reinterpret_cast<wchar_t *>(reinterpret_cast<std::byte *>(request.Item) + sizeof(PluginPanelItem));
            std::memcpy(name, itemName, sizeof(itemName));
            request.Item->FileName = name;
            request.Item->Flags = PPIF_SELECTED;
            return bytes;
        }
        if (sameFarPanels && command == FCTL_GETPANELDIRECTORY) {
            constexpr wchar_t activeDirectory[] = L"C:\\source";
            constexpr wchar_t passiveDirectory[] = L"D:\\destination";
            const auto source =
                panel == PANEL_ACTIVE ? std::wstring_view{activeDirectory} : std::wstring_view{passiveDirectory};
            const auto bytes = sizeof(FarPanelDirectory) + (source.size() + 1) * sizeof(wchar_t);
            if (parameter == nullptr) {
                return static_cast<intptr_t>(bytes);
            }
            auto &directory = *static_cast<FarPanelDirectory *>(parameter);
            auto *name =
                reinterpret_cast<wchar_t *>(reinterpret_cast<std::byte *>(parameter) + sizeof(FarPanelDirectory));
            std::memcpy(name, source.data(), source.size() * sizeof(wchar_t));
            name[source.size()] = L'\0';
            directory.Name = name;
            return static_cast<intptr_t>(bytes);
        }
        if (receivePanel && command == FCTL_GETPANELDIRECTORY) {
            if (parameter == nullptr) {
                return sizeof(FarPanelDirectory);
            }
            auto &directory = *static_cast<FarPanelDirectory *>(parameter);
            directory.Name = receiveDirectory.c_str();
            return 1;
        }
        if (receivePanel && command == FCTL_UPDATEPANEL) {
            ++panelUpdates;
            return 1;
        }
        if (receivePanel && command == FCTL_REDRAWPANEL) {
            ++panelRedraws;
            return 1;
        }
        return 0;
    }

    intptr_t WINAPI advControl(const UUID *, ADVANCED_CONTROL_COMMANDS command, intptr_t, void *parameter)
    {
        if (throwFromFar) {
            throw std::runtime_error{"firewall"};
        }
        if (command == ACTL_SYNCHRO) {
            ++synchros;
            if (dispatchSynchro) {
                ProcessSynchroEventInfo event{};
                event.Event = SE_COMMONSYNCHRO;
                static_cast<void>(ProcessSynchroEventW(&event));
            }
            return 1;
        }
        auto &window = *static_cast<WindowInfo *>(parameter);
        window.Type = WTYPE_PANELS;
        return 1;
    }

    PluginStartupInfo startup()
    {
        PluginStartupInfo info{};
        info.StructSize = sizeof(info);
        info.PanelControl = panelControl;
        info.AdvControl = advControl;
        return info;
    }

    ProcessConsoleInputInfo inputInfo(COORD cell, DWORD buttons, DWORD flags = 0)
    {
        ProcessConsoleInputInfo info{};
        info.StructSize = sizeof(info);
        info.Rec.EventType = MOUSE_EVENT;
        info.Rec.Event.MouseEvent.dwMousePosition = cell;
        info.Rec.Event.MouseEvent.dwButtonState = buttons;
        info.Rec.Event.MouseEvent.dwEventFlags = flags;
        return info;
    }

    class TestScreen final : public burlak::core::IScreen
    {
      public:
        burlak::core::NativeWindow hostHandle{};
        std::optional<burlak::core::Point> point;
        burlak::core::NativeWindow root{};
        bool leftDown{};
        [[nodiscard]] std::optional<burlak::core::Point> cursor() override
        {
            return point;
        }
        [[nodiscard]] bool buttonDown(burlak::core::Button button) override
        {
            return button == burlak::core::Button::Left && leftDown;
        }
        [[nodiscard]] burlak::core::NativeWindow windowAt(burlak::core::Point) override
        {
            return root;
        }
        [[nodiscard]] burlak::core::NativeWindow consoleWindow() override
        {
            return hostHandle;
        }
        [[nodiscard]] burlak::core::NativeWindow hostWindowHandle() override
        {
            return hostHandle;
        }
        [[nodiscard]] std::optional<burlak::core::HostWindow> hostWindow() override
        {
            return burlak::core::HostWindow{hostHandle, {0, 0, 80, 25}, false};
        }
        [[nodiscard]] std::optional<burlak::core::HostWindow> hostWindowAt(burlak::core::Point) override
        {
            return hostWindow();
        }
        [[nodiscard]] std::expected<burlak::core::CellGeometry, burlak::core::Error> cellGeometry() override
        {
            return burlak::core::CellGeometry{{0, 0}, 1, 1};
        }
        [[nodiscard]] std::expected<burlak::core::CellGeometry, burlak::core::Error> cellGeometryAt(
            burlak::core::Point) override
        {
            return cellGeometry();
        }
    };

    class TestShell final : public burlak::core::IShell
    {
      public:
        class Data final : public DragData
        {
          public:
            [[nodiscard]] std::uintptr_t nativeHandle() const override
            {
                return 0;
            }
        };

        std::vector<std::wstring> paths;
        std::wstring destination;
        burlak::core::Effect effect{burlak::core::Effect::None};
        burlak::core::NativeWindow owner{};

        [[nodiscard]] std::expected<PreparedDrag, burlak::core::Error> makeDataObject(
            std::span<const std::wstring>, std::optional<burlak::core::Effect>) override
        {
            return std::unexpected(burlak::core::Error::Unavailable);
        }
        [[nodiscard]] burlak::core::DragLoopOutcome runDrag(burlak::core::NativeWindow, DragData &, std::uintptr_t,
                                                            bool) override
        {
            return {};
        }
        [[nodiscard]] std::expected<void, burlak::core::Error> copy(std::span<const std::wstring> source,
                                                                    std::wstring_view target,
                                                                    burlak::core::Effect chosen,
                                                                    burlak::core::NativeWindow host) override
        {
            paths.assign(source.begin(), source.end());
            destination = target;
            effect = chosen;
            owner = host;
            return {};
        }
    };

    class TestFiles final : public burlak::core::IFiles
    {
      public:
        int sweeps{};

        [[nodiscard]] std::expected<std::wstring, burlak::core::Error> runDirectory() override
        {
            return std::unexpected(burlak::core::Error::Unavailable);
        }
        [[nodiscard]] std::expected<std::wstring, burlak::core::Error> placeholder(std::wstring_view, bool) override
        {
            return std::unexpected(burlak::core::Error::Unavailable);
        }
        [[nodiscard]] bool nameBefore(std::wstring_view, std::wstring_view) const override
        {
            return false;
        }
        [[nodiscard]] std::expected<void, burlak::core::Error> removeTree(std::wstring_view) override
        {
            return {};
        }
        [[nodiscard]] std::expected<void, burlak::core::Error> touch(std::wstring_view) override
        {
            return {};
        }
        [[nodiscard]] bool processAlive(std::uint32_t) const override
        {
            return true;
        }
        void sweep() override
        {
            ++sweeps;
        }
    };

    class TestProperties final : public burlak::core::IWindowProperties
    {
      public:
        std::optional<std::uint32_t> stored;
        burlak::core::NativeWindow window{};
        int removes{};

        void set(burlak::core::NativeWindow target, std::uint32_t value) override
        {
            window = target;
            stored = value;
        }
        [[nodiscard]] std::optional<std::uint32_t> value(burlak::core::NativeWindow) const override
        {
            return stored;
        }
        void remove(burlak::core::NativeWindow target) override
        {
            window = target;
            stored.reset();
            ++removes;
        }
        [[nodiscard]] std::uint32_t processId() const override
        {
            return 7;
        }
    };

    class TestMenu final : public burlak::core::IDropMenu
    {
      public:
        burlak::core::DropMenuChoice choice{burlak::core::DropMenuChoice::Cancel};
        std::optional<burlak::core::NativeWindow> owner;

        [[nodiscard]] burlak::core::DropMenuChoice choose(burlak::core::NativeWindow menuOwner, burlak::core::Point,
                                                          burlak::core::AllowedEffects) override
        {
            owner = menuOwner;
            return choice;
        }
    };

    class SameFarScreen final : public burlak::core::IScreen
    {
      public:
        [[nodiscard]] std::optional<burlak::core::Point> cursor() override
        {
            return sameFarDropPoint;
        }
        [[nodiscard]] bool buttonDown(burlak::core::Button) override
        {
            return true;
        }
        [[nodiscard]] burlak::core::NativeWindow windowAt(burlak::core::Point) override
        {
            return burlak::plugin::composition().toolWindow();
        }
        [[nodiscard]] burlak::core::NativeWindow consoleWindow() override
        {
            return 99;
        }
        [[nodiscard]] burlak::core::NativeWindow hostWindowHandle() override
        {
            return 0;
        }
        [[nodiscard]] std::optional<burlak::core::HostWindow> hostWindow() override
        {
            return burlak::core::HostWindow{99, {0, 0, 80, 25}, false};
        }
        [[nodiscard]] std::optional<burlak::core::HostWindow> hostWindowAt(burlak::core::Point) override
        {
            return hostWindow();
        }
        [[nodiscard]] std::expected<burlak::core::CellGeometry, burlak::core::Error> cellGeometry() override
        {
            return burlak::core::CellGeometry{{0, 0}, 1, 1};
        }
        [[nodiscard]] std::expected<burlak::core::CellGeometry, burlak::core::Error> cellGeometryAt(
            burlak::core::Point) override
        {
            return cellGeometry();
        }
    };

    class SameFarShell final : public burlak::core::IShell
    {
      public:
        class Data final : public DragData
        {
          public:
            [[nodiscard]] std::uintptr_t nativeHandle() const override
            {
                return 0;
            }
        };

        burlak::core::NativeWindow dragOwner{};

        [[nodiscard]] std::expected<PreparedDrag, burlak::core::Error> makeDataObject(
            std::span<const std::wstring> paths, std::optional<burlak::core::Effect>) override
        {
            return PreparedDrag{std::make_unique<Data>(), paths.size()};
        }
        [[nodiscard]] burlak::core::DragLoopOutcome runDrag(burlak::core::NativeWindow owner, DragData &,
                                                            std::uintptr_t, bool) override
        {
            dragOwner = owner;
            burlak::plugin::composition().dropOnToolWindow(sameFarDropPoint, true);
            return {.status = DRAGDROP_S_DROP, .effect = DROPEFFECT_MOVE};
        }
        [[nodiscard]] std::expected<void, burlak::core::Error> copy(std::span<const std::wstring>, std::wstring_view,
                                                                    burlak::core::Effect,
                                                                    burlak::core::NativeWindow) override
        {
            return std::unexpected(burlak::core::Error::Unavailable);
        }
    };

} // namespace

TEST_SUITE("plugin exports")
{
    TEST_CASE("global and plugin metadata report 1.3.0")
    {
        GlobalInfo global{};
        GetGlobalInfoW(&global);
        CHECK(global.StructSize == sizeof(global));
        CHECK(global.MinFarVersion.Major == 3);
        CHECK(global.MinFarVersion.Minor == 0);
        CHECK(global.MinFarVersion.Revision == 0);
        CHECK(global.MinFarVersion.Build == 2843);
        CHECK(global.Version.Major == 1);
        CHECK(global.Version.Minor == 3);
        CHECK(global.Version.Revision == 0);
        CHECK(std::wstring_view{global.Title} == L"Burlak");
        CHECK(std::wstring_view{global.Author} == L"Roman Kharitonov");
        CHECK(std::wstring_view{global.Description} == L"Drag files out of the panel into any drop target");

        PluginInfo plugin{};
        GetPluginInfoW(&plugin);
        CHECK(plugin.StructSize == sizeof(plugin));
        CHECK(plugin.Flags == PF_NONE);
        CHECK(OpenW(nullptr) == nullptr);
    }

    TEST_CASE("the composition-owned extraction event pumps until Far reports success or failure")
    {
        ExtractionSession session;
        const auto calls = waitCalls();
        burlak::drag::ExtractionWait wait{session, calls};
        activeWait = &wait;
        resetStatus = TRUE;
        waitStatus = S_OK;
        waitIndex = 0;
        waitOutcome = true;
        CHECK(wait.extract());
        CHECK(session.requests == 1);

        waitOutcome = false;
        CHECK_FALSE(wait.extract());
        CHECK(session.requests == 2);

        bool nestedResult{true};
        waitAction = [&] {
            nestedResult = activeWait->extract();
            activeWait->complete(true);
        };
        CHECK(wait.extract());
        CHECK_FALSE(nestedResult);

        waitAction = [&] {
            burlak::drag::ExtractionCompleter completion{*activeWait};
            completion.complete(true);
        };
        CHECK(wait.extract());
        wait.cleanup();
        CHECK(session.cleanups == 1);
        wait.retain();
        CHECK(session.retained == 1);

        session.accepts = false;
        CHECK_FALSE(wait.extract());
        session.accepts = true;
        resetStatus = FALSE;
        CHECK_FALSE(wait.extract());
        resetStatus = TRUE;
        waitStatus = E_FAIL;
        CHECK_FALSE(wait.extract());
        waitStatus = S_OK;
        waitIndex = 1;
        CHECK_FALSE(wait.extract());
        wait.complete(std::nullopt);
        wait.complete(std::optional{true});

        auto missingCalls = calls;
        missingCalls.createEvent = failCreateEvent;
        burlak::drag::ExtractionWait missing{session, missingCalls};
        CHECK_FALSE(missing.extract());

        burlak::drag::ExtractionWait cancelled{session, calls};
        activeWait = &cancelled;
        waitAction = [&] { activeWait->cancel(); };
        CHECK_FALSE(cancelled.extract());
        cancelled.cancel();
        CHECK(burlak::drag::systemExtractionWaitCalls().coWait == CoWaitForMultipleHandles);
    }

    TEST_CASE("mouse marshalling preserves the left and right 1.2.0 verdicts")
    {
        auto startupInfo = startup();
        SetStartupInfoW(&startupInfo);

        auto left = inputInfo({5, 5}, FROM_LEFT_1ST_BUTTON_PRESSED);
        CHECK(ProcessConsoleInputW(&left) == 0);
        auto shortMove = inputInfo({6, 5}, FROM_LEFT_1ST_BUTTON_PRESSED, MOUSE_MOVED);
        CHECK(ProcessConsoleInputW(&shortMove) == 1);
        auto threshold = inputInfo({8, 5}, FROM_LEFT_1ST_BUTTON_PRESSED, MOUSE_MOVED);
        CHECK(ProcessConsoleInputW(&threshold) == 2);
        CHECK(threshold.Rec.Event.MouseEvent.dwMousePosition.X == 5);
        CHECK(threshold.Rec.Event.MouseEvent.dwButtonState == 0);
        CHECK(threshold.Rec.Event.MouseEvent.dwEventFlags == 0);
        CHECK(synchros == 1);

        ProcessSynchroEventInfo wrong{};
        wrong.Event = SE_FOLDERCHANGED;
        CHECK(ProcessSynchroEventW(nullptr) == 0);
        CHECK(ProcessSynchroEventW(&wrong) == 0);
        ProcessSynchroEventInfo common{};
        common.Event = SE_COMMONSYNCHRO;
        CHECK(ProcessSynchroEventW(&common) == 0);

        auto right = inputInfo({5, 5}, RIGHTMOST_BUTTON_PRESSED);
        CHECK(ProcessConsoleInputW(&right) == 1);
        auto release = inputInfo({6, 6}, 0);
        CHECK(ProcessConsoleInputW(&release) == 2);
        CHECK(release.Rec.Event.MouseEvent.dwMousePosition.X == 5);
        CHECK(release.Rec.Event.MouseEvent.dwButtonState == RIGHTMOST_BUTTON_PRESSED);

        right = inputInfo({5, 5}, RIGHTMOST_BUTTON_PRESSED);
        right.Rec.Event.MouseEvent.dwControlKeyState = SHIFT_PRESSED | LEFT_CTRL_PRESSED | LEFT_ALT_PRESSED;
        CHECK(ProcessConsoleInputW(&right) == 1);
        auto rightThreshold = inputInfo({8, 5}, RIGHTMOST_BUTTON_PRESSED, MOUSE_MOVED);
        rightThreshold.Rec.Event.MouseEvent.dwControlKeyState = SHIFT_PRESSED | LEFT_CTRL_PRESSED | LEFT_ALT_PRESSED;
        CHECK(ProcessConsoleInputW(&rightThreshold) == 2);
        CHECK(rightThreshold.Rec.Event.MouseEvent.dwButtonState == FROM_LEFT_1ST_BUTTON_PRESSED);
        CHECK(rightThreshold.Rec.Event.MouseEvent.dwEventFlags == MOUSE_MOVED);
        CHECK(rightThreshold.Rec.Event.MouseEvent.dwControlKeyState ==
              (SHIFT_PRESSED | LEFT_CTRL_PRESSED | LEFT_ALT_PRESSED));
    }

    TEST_CASE("startup and exit sweep once each while focus marks and clears the host receiver")
    {
        TestScreen screen;
        TestShell shell;
        TestFiles files;
        TestProperties properties;
        TestMenu menu;
        burlak::plugin::composition().useReceiveAdapters(screen, shell, files, properties, menu);
        auto startupInfo = startup();
        SetStartupInfoW(&startupInfo);
        CHECK(burlak::plugin::composition().toolWindow() != 0);
        CHECK(files.sweeps == 1);

        ProcessConsoleInputInfo focus{};
        focus.StructSize = sizeof(focus);
        focus.Rec.EventType = FOCUS_EVENT;
        focus.Rec.Event.FocusEvent.bSetFocus = FALSE;
        CHECK(ProcessConsoleInputW(&focus) == 0);
        CHECK_FALSE(properties.stored.has_value());
        focus.Rec.Event.FocusEvent.bSetFocus = TRUE;
        CHECK(ProcessConsoleInputW(&focus) == 0);
        CHECK_FALSE(properties.stored.has_value());
        screen.hostHandle = 99;
        CHECK(ProcessConsoleInputW(&focus) == 0);
        CHECK(properties.stored == 7);
        CHECK(properties.window == 99);
        ExitFARW(nullptr);
        CHECK_FALSE(properties.stored.has_value());
        CHECK(properties.removes == 1);
        CHECK(files.sweeps == 2);
        burlak::plugin::composition().useDefaultAdapters();
    }

    TEST_CASE("external copy and Shift-move drops cross the export synchro boundary and redraw the panel")
    {
        REQUIRE(SUCCEEDED(OleInitialize(nullptr)));
        TemporaryDropFiles filesOnDisk;
        const auto paths = filesOnDisk.paths();
        auto data = burlak::adapters::shell::makeDataObject(paths);
        REQUIRE(data.has_value());
        TestScreen screen;
        TestShell shell;
        TestFiles files;
        TestProperties properties;
        TestMenu menu;
        burlak::plugin::composition().useReceiveAdapters(screen, shell, files, properties, menu);
        receivePanel = true;
        receiveDirectory = L"D:\\destination";
        panelUpdates = 0;
        panelRedraws = 0;
        synchros = 0;
        auto startupInfo = startup();
        SetStartupInfoW(&startupInfo);
        const auto tool = burlak::plugin::composition().toolWindow();
        REQUIRE(tool != 0);

        screen.hostHandle = 99;
        screen.leftDown = true;
        screen.point = burlak::core::Point{1, 1};
        screen.root = 30;
        SendMessageW(reinterpret_cast<HWND>(tool), WM_TIMER, burlak::drag::externalDragPollTimerId(), 0);
        const auto before = synchros.load();
        screen.point = burlak::core::Point{5, 5};
        screen.root = 99;
        SendMessageW(reinterpret_cast<HWND>(tool), WM_TIMER, burlak::drag::externalDragPollTimerId(), 0);
        CHECK(synchros == before + 1);

        ProcessSynchroEventInfo event{};
        event.Event = SE_COMMONSYNCHRO;
        CHECK(ProcessSynchroEventW(&event) == 0);
        CHECK(burlak::plugin::composition().toolWindow() == tool);
        dispatchSynchro = true;

        bool moving{};
        bool rightButton{};
        bool declined{};
        SUBCASE("copy")
        {
            moving = false;
        }
        SUBCASE("Shift move")
        {
            moving = true;
        }
        SUBCASE("right-button menu copy")
        {
            rightButton = true;
            menu.choice = burlak::core::DropMenuChoice::Copy;
        }
        SUBCASE("right-button menu cancelled")
        {
            rightButton = true;
            declined = true;
            menu.choice = burlak::core::DropMenuChoice::Cancel;
        }
        const auto nativeData = reinterpret_cast<std::uintptr_t>(data->data.Get());
        const auto keys = moving ? static_cast<std::uint32_t>(MK_SHIFT) : (rightButton ? MK_RBUTTON : 0U);
        const auto expectedHover = moving ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
        CHECK(burlak::plugin::composition().dragEnterToolWindow(nativeData, keys, {5, 5},
                                                                DROPEFFECT_COPY | DROPEFFECT_MOVE) == expectedHover);
        screen.leftDown = false;
        const auto returned =
            burlak::plugin::composition().dropOnToolWindow(nativeData, keys, {5, 5}, DROPEFFECT_COPY | DROPEFFECT_MOVE);
        // A move is announced through CFSTR_PERFORMEDDROPEFFECT = NONE and still answers COPY: a NONE return makes
        // ole32 treat the drop as refused and paste the path into the console that owns the overlay.
        CHECK(returned == DROPEFFECT_COPY);
        // The popup must be owned by the tool window of this thread: TrackPopupMenu refuses the host, which belongs
        // to conhost or Windows Terminal, and would otherwise cancel every right-button drop.
        CHECK(menu.owner == (rightButton ? std::optional{tool} : std::nullopt));
        if (declined) {
            // The user was offered the menu and said no: nothing is copied or redrawn, and OLE is still answered COPY
            // so that nothing is pasted into the console either.
            CHECK(shell.paths.empty());
            CHECK(shell.effect == burlak::core::Effect::None);
            CHECK(panelUpdates == 0);
            CHECK(panelRedraws == 0);
        } else {
            // The receiver copies exactly the paths the data object names; the shell names them canonically.
            CHECK(TemporaryDropFiles::canonical(shell.paths) == TemporaryDropFiles::canonical(paths));
            CHECK(shell.destination == receiveDirectory);
            CHECK(shell.effect == (moving ? burlak::core::Effect::Move : burlak::core::Effect::Copy));
            CHECK(shell.owner == 99);
            CHECK(panelUpdates == 1);
            CHECK(panelRedraws == 1);
        }

        const auto formatId = RegisterClipboardFormatW(CFSTR_PERFORMEDDROPEFFECT);
        REQUIRE(formatId != 0);
        FORMATETC format{static_cast<CLIPFORMAT>(formatId), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium{};
        const auto getData = data->data->GetData(&format, &medium);
        CHECK((moving ? SUCCEEDED(getData) : getData == DV_E_FORMATETC));
        if (SUCCEEDED(getData)) {
            const auto value = static_cast<const DWORD *>(GlobalLock(medium.hGlobal));
            REQUIRE(value != nullptr);
            CHECK(*value == DROPEFFECT_NONE);
            GlobalUnlock(medium.hGlobal);
            ReleaseStgMedium(&medium);
        }

        ExitFARW(nullptr);
        dispatchSynchro = false;
        receivePanel = false;
        burlak::plugin::composition().useDefaultAdapters();
        OleUninitialize();
    }

    TEST_CASE("right double-click replay preserves every native mouse-record field")
    {
        auto startupInfo = startup();
        SetStartupInfoW(&startupInfo);

        constexpr DWORD buttons = RIGHTMOST_BUTTON_PRESSED | FROM_LEFT_2ND_BUTTON_PRESSED;
        constexpr DWORD controls = RIGHT_CTRL_PRESSED | RIGHT_ALT_PRESSED | CAPSLOCK_ON | NUMLOCK_ON;
        auto press = inputInfo({9, 7}, buttons, DOUBLE_CLICK);
        press.Rec.Event.MouseEvent.dwControlKeyState = controls;
        const auto original = press.Rec.Event.MouseEvent;
        CHECK(ProcessConsoleInputW(&press) == 1);

        auto release = inputInfo({10, 8}, 0);
        CHECK(ProcessConsoleInputW(&release) == 2);
        CHECK(release.Rec.Event.MouseEvent.dwMousePosition.X == original.dwMousePosition.X);
        CHECK(release.Rec.Event.MouseEvent.dwMousePosition.Y == original.dwMousePosition.Y);
        CHECK(release.Rec.Event.MouseEvent.dwButtonState == original.dwButtonState);
        CHECK(release.Rec.Event.MouseEvent.dwControlKeyState == original.dwControlKeyState);
        CHECK(release.Rec.Event.MouseEvent.dwEventFlags == original.dwEventFlags);
    }

    TEST_CASE("Far's replayed panel press and release both pass through unchanged")
    {
        auto startupInfo = startup();
        SetStartupInfoW(&startupInfo);

        auto press = inputInfo({5, 5}, FROM_LEFT_1ST_BUTTON_PRESSED);
        press.Rec.Event.MouseEvent.dwControlKeyState = SHIFT_PRESSED;
        const auto originalPress = press.Rec.Event.MouseEvent;
        CHECK(ProcessConsoleInputW(&press) == 0);
        CHECK(std::memcmp(&press.Rec.Event.MouseEvent, &originalPress, sizeof(originalPress)) == 0);

        auto release = inputInfo({45, 5}, 0);
        release.Rec.Event.MouseEvent.dwControlKeyState = SHIFT_PRESSED;
        const auto originalRelease = release.Rec.Event.MouseEvent;
        CHECK(ProcessConsoleInputW(&release) == 0);
        CHECK(std::memcmp(&release.Rec.Event.MouseEvent, &originalRelease, sizeof(originalRelease)) == 0);
    }

    TEST_CASE("a same-Far drop through the exports replays buffer-relative press and release records")
    {
        SameFarScreen screen;
        SameFarShell shell;
        TestFiles files;
        TestProperties properties;
        TestMenu menu;
        const burlak::adapters::win::InputCalls calls{replayMouseEvent, replayConsoleInfo, captureConsoleInput};
        burlak::adapters::win::Input input{11, 12, calls};
        burlak::plugin::composition().useReceiveAdapters(screen, shell, files, properties, menu);
        burlak::plugin::composition().useInputAdapter(input);
        sameFarPanels = true;
        synchros = 0;
        replayedRecords.clear();
        auto startupInfo = startup();
        SetStartupInfoW(&startupInfo);

        auto press = inputInfo({5, 5}, FROM_LEFT_1ST_BUTTON_PRESSED);
        press.Rec.Event.MouseEvent.dwControlKeyState = SHIFT_PRESSED;
        CHECK(ProcessConsoleInputW(&press) == 0);
        auto threshold = inputInfo({8, 5}, FROM_LEFT_1ST_BUTTON_PRESSED, MOUSE_MOVED);
        threshold.Rec.Event.MouseEvent.dwControlKeyState = SHIFT_PRESSED;
        REQUIRE(ProcessConsoleInputW(&threshold) == 2);
        CHECK(synchros == 1);

        ProcessSynchroEventInfo event{};
        event.Event = SE_COMMONSYNCHRO;
        CHECK(ProcessSynchroEventW(&event) == 0);
        CHECK(shell.dragOwner == burlak::plugin::composition().toolWindow());
        CHECK(synchros == 2);
        CHECK(ProcessSynchroEventW(&event) == 0);

        REQUIRE(replayedRecords.size() == 2);
        const auto &replayedPress = replayedRecords[0].Event.MouseEvent;
        CHECK(replayedPress.dwMousePosition.X == 5);
        CHECK(replayedPress.dwMousePosition.Y == 8956);
        CHECK(replayedPress.dwButtonState == FROM_LEFT_1ST_BUTTON_PRESSED);
        CHECK(replayedPress.dwControlKeyState == SHIFT_PRESSED);
        CHECK(replayedPress.dwEventFlags == 0);
        const auto &replayedRelease = replayedRecords[1].Event.MouseEvent;
        CHECK(replayedRelease.dwMousePosition.X == sameFarDropPoint.x);
        CHECK(replayedRelease.dwMousePosition.Y == sameFarDropPoint.y + 8951);
        CHECK(replayedRelease.dwButtonState == 0);
        CHECK(replayedRelease.dwControlKeyState == SHIFT_PRESSED);
        CHECK(replayedRelease.dwEventFlags == 0);

        ExitFARW(nullptr);
        sameFarPanels = false;
        burlak::plugin::composition().useDefaultAdapters();
    }

    TEST_CASE("non-mouse input, null input, null startup, and Far exceptions stay behind the firewall")
    {
        SetStartupInfoW(nullptr);
        CHECK(ProcessConsoleInputW(nullptr) == 0);
        ProcessConsoleInputInfo key{};
        key.Rec.EventType = KEY_EVENT;
        CHECK(ProcessConsoleInputW(&key) == 0);

        auto startupInfo = startup();
        SetStartupInfoW(&startupInfo);
        throwFromFar = true;
        auto press = inputInfo({5, 5}, FROM_LEFT_1ST_BUTTON_PRESSED);
        CHECK(ProcessConsoleInputW(&press) == 0);
        throwFromFar = false;

        ExitFARW(nullptr);

        burlak::plugin::Composition empty;
        CHECK(empty.feed({}).action == burlak::core::VerdictAction::Pass);
        empty.recordFocus();
        CHECK(empty.toolWindow() == 0);
        empty.dropOnToolWindow({}, false);
        CHECK(empty.dragEnterToolWindow(0, 0, {}, DROPEFFECT_COPY) == 0);
        CHECK(empty.dropOnToolWindow(0, 0, {}, DROPEFFECT_COPY) == 0);
        empty.synchro();
        empty.stop();
        empty.useDefaultAdapters();
        burlak::plugin::firewall([] { throw std::runtime_error{"firewall"}; });
        burlak::plugin::firewall([] {});
    }
}
