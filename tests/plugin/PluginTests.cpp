#include "plugin/Composition.hpp"
#include "plugin/Firewall.hpp"

#include <doctest/doctest.h>

#include <plugin.hpp>

#include <stdexcept>

namespace
{

    int synchros{};
    bool throwFromFar{};

    intptr_t WINAPI panelControl(HANDLE panel, FILE_CONTROL_COMMANDS command, intptr_t, void *parameter)
    {
        if (command == FCTL_GETPANELINFO) {
            auto &info = *static_cast<PanelInfo *>(parameter);
            info.Flags = panel == PANEL_ACTIVE ? PFLAGS_VISIBLE | PFLAGS_REALNAMES : PFLAGS_NONE;
            info.PanelRect = RECT{0, 0, 39, 24};
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

} // namespace

TEST_SUITE("plugin exports")
{
    TEST_CASE("global and plugin metadata remain 1.2.0")
    {
        GlobalInfo global{};
        GetGlobalInfoW(&global);
        CHECK(global.StructSize == sizeof(global));
        CHECK(global.MinFarVersion.Major == 3);
        CHECK(global.MinFarVersion.Minor == 0);
        CHECK(global.MinFarVersion.Revision == 0);
        CHECK(global.MinFarVersion.Build == 2843);
        CHECK(global.Version.Major == 1);
        CHECK(global.Version.Minor == 2);
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
        empty.synchro();
        empty.stop();
        burlak::plugin::firewall([] { throw std::runtime_error{"firewall"}; });
        burlak::plugin::firewall([] {});
    }
}
