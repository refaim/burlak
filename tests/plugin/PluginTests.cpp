#include "adapters/win/Peers.hpp"
#include "core/PeerWire.hpp"
#include "drag/ExtractionWait.hpp"
#include "plugin/Composition.hpp"
#include "plugin/Firewall.hpp"

#include <doctest/doctest.h>

#include <plugin.hpp>

#include <cstring>
#include <stdexcept>

namespace
{

    int synchros{};
    bool throwFromFar{};
    bool peerPanel{};
    int panelUpdates{};
    int panelRedraws{};
    std::wstring peerDirectory;

    class ExtractionSession final : public burlak::core::IExtractionSession
    {
      public:
        bool accepts{true};
        int requests{};
        int cleanups{};

        [[nodiscard]] bool requestExtraction() override
        {
            ++requests;
            return accepts;
        }

        void cleanup() override
        {
            ++cleanups;
        }
    };

    burlak::drag::ExtractionWait *activeWait{};
    bool waitOutcome{true};
    HRESULT waitStatus{S_OK};
    DWORD waitIndex{};
    BOOL resetStatus{TRUE};

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
            activeWait->complete(waitOutcome);
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
            info.Flags = panel == PANEL_ACTIVE ? PFLAGS_VISIBLE | PFLAGS_REALNAMES : PFLAGS_NONE;
            info.PanelType = PTYPE_FILEPANEL;
            info.PanelRect = RECT{0, 0, 39, 24};
            return 1;
        }
        if (peerPanel && command == FCTL_GETPANELDIRECTORY) {
            if (parameter == nullptr) {
                return sizeof(FarPanelDirectory);
            }
            auto &directory = *static_cast<FarPanelDirectory *>(parameter);
            directory.Name = peerDirectory.c_str();
            return 1;
        }
        if (peerPanel && command == FCTL_UPDATEPANEL) {
            ++panelUpdates;
            return 1;
        }
        if (peerPanel && command == FCTL_REDRAWPANEL) {
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

    class PeerScreen final : public burlak::core::IScreen
    {
      public:
        [[nodiscard]] std::optional<burlak::core::Point> cursor() override
        {
            return std::nullopt;
        }
        [[nodiscard]] bool buttonDown(burlak::core::Button) override
        {
            return false;
        }
        [[nodiscard]] burlak::core::NativeWindow windowAt(burlak::core::Point) override
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

    class PeerShell final : public burlak::core::IShell
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
            std::span<const std::wstring>) override
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
        wait.cleanup();
        CHECK(session.cleanups == 1);

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
        CHECK(burlak::drag::systemExtractionWaitCalls().coWait == CoWaitForMultipleHandles);
    }

    TEST_CASE("mouse marshalling preserves the left and right 1.2.0 verdicts")
    {
        auto startupInfo = startup();
        SetStartupInfoW(&startupInfo);

        auto left = inputInfo({5, 5}, FROM_LEFT_1ST_BUTTON_PRESSED);
        CHECK(ProcessConsoleInputW(&left) == 0);
        auto shortMove = inputInfo({7, 5}, FROM_LEFT_1ST_BUTTON_PRESSED, MOUSE_MOVED);
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

    TEST_CASE("startup creates the idle peer responder and focus records update its tick")
    {
        auto startupInfo = startup();
        SetStartupInfoW(&startupInfo);
        CHECK(burlak::plugin::composition().toolWindow() != 0);
        CHECK(burlak::plugin::composition().lastFocus() == 0);

        ProcessConsoleInputInfo focus{};
        focus.StructSize = sizeof(focus);
        focus.Rec.EventType = FOCUS_EVENT;
        focus.Rec.Event.FocusEvent.bSetFocus = FALSE;
        CHECK(ProcessConsoleInputW(&focus) == 0);
        CHECK(burlak::plugin::composition().lastFocus() == 0);
        focus.Rec.Event.FocusEvent.bSetFocus = TRUE;
        CHECK(ProcessConsoleInputW(&focus) == 0);
        CHECK(burlak::plugin::composition().lastFocus() != 0);
    }

    TEST_CASE("a peer wire drop reaches Far synchro, shell copy, and panel redraw through the exports")
    {
        PeerScreen screen;
        PeerShell shell;
        burlak::plugin::composition().usePeerDropAdapters(screen, shell);
        const std::wstring source{L"C:\\source\\received.txt"};
        const std::wstring destination{L"D:\\destination"};

        peerPanel = true;
        peerDirectory = destination;
        panelUpdates = 0;
        panelRedraws = 0;
        auto startupInfo = startup();
        SetStartupInfoW(&startupInfo);
        const auto tool = burlak::plugin::composition().toolWindow();
        REQUIRE(tool != 0);
        const auto bytes =
            burlak::core::encodePeerDrop({.paths = {source}, .at = {5, 5}, .effect = burlak::core::Effect::Copy});
        COPYDATASTRUCT copy{.dwData = burlak::adapters::win::peerDropDataKind(),
                            .cbData = static_cast<DWORD>(bytes.size()),
                            .lpData = const_cast<std::byte *>(bytes.data())};
        const auto before = synchros;
        REQUIRE(SendMessageW(reinterpret_cast<HWND>(tool), WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copy)) == 1);
        CHECK(synchros == before + 1);

        ProcessSynchroEventInfo event{};
        event.Event = SE_COMMONSYNCHRO;
        CHECK(ProcessSynchroEventW(&event) == 0);
        CHECK(shell.paths == std::vector<std::wstring>{source});
        CHECK(shell.destination == destination);
        CHECK(shell.effect == burlak::core::Effect::Copy);
        CHECK(shell.owner == 99);
        CHECK(panelUpdates == 1);
        CHECK(panelRedraws == 1);

        ExitFARW(nullptr);
        peerPanel = false;
        burlak::plugin::composition().useDefaultAdapters();
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
        CHECK(empty.lastFocus() == 0);
        CHECK(empty.toolWindow() == 0);
        empty.synchro();
        empty.stop();
        empty.useDefaultAdapters();
        burlak::plugin::firewall([] { throw std::runtime_error{"firewall"}; });
        burlak::plugin::firewall([] {});
    }
}
