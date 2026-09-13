#include "plugin/Composition.hpp"

#include "adapters/far/FarApi.hpp"
#include "plugin/Firewall.hpp"
#include "plugin/version.h"

#include <windows.h>

#include <plugin.hpp>

namespace
{

    [[nodiscard]] burlak::core::MouseEvent toCore(const MOUSE_EVENT_RECORD &mouse)
    {
        const DWORD controls = mouse.dwControlKeyState;
        return {.at = {mouse.dwMousePosition.X, mouse.dwMousePosition.Y},
                .left = (mouse.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0,
                .right = (mouse.dwButtonState & RIGHTMOST_BUTTON_PRESSED) != 0,
                .moved = (mouse.dwEventFlags & MOUSE_MOVED) != 0,
                .wheel = (mouse.dwEventFlags & (MOUSE_WHEELED | MOUSE_HWHEELED)) != 0,
                .mods = {.shift = (controls & SHIFT_PRESSED) != 0,
                         .control = (controls & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0,
                         .alt = (controls & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0},
                .nativeButtonState = mouse.dwButtonState,
                .nativeControlState = mouse.dwControlKeyState,
                .nativeEventFlags = mouse.dwEventFlags};
    }

    void fromCore(const burlak::core::MouseEvent &source, MOUSE_EVENT_RECORD &destination)
    {
        destination.dwMousePosition = {static_cast<SHORT>(source.at.x), static_cast<SHORT>(source.at.y)};
        destination.dwButtonState = source.nativeButtonState;
        destination.dwControlKeyState = source.nativeControlState;
        destination.dwEventFlags = source.nativeEventFlags;
        if (source.rewrite == burlak::core::MouseEvent::Rewrite::ButtonlessRelease) {
            destination.dwButtonState = 0;
            destination.dwEventFlags = 0;
        } else if (source.rewrite == burlak::core::MouseEvent::Rewrite::LeftHeldMove) {
            destination.dwButtonState = FROM_LEFT_1ST_BUTTON_PRESSED;
            destination.dwEventFlags = MOUSE_MOVED;
        }
    }

    struct GlobalInfoCall
    {
        GlobalInfo *info;
        void run() const
        {
            info->StructSize = sizeof(*info);
            info->MinFarVersion = MAKEFARVERSION(3, 0, 0, 2843, VS_RELEASE);
            info->Version =
                MAKEFARVERSION(BURLAK_VERSION_MAJOR, BURLAK_VERSION_MINOR, BURLAK_VERSION_PATCH, 0, VS_RELEASE);
            info->Guid = burlak::adapters::far_api::pluginGuid();
            info->Title = L"Burlak";
            info->Description = L"Drag files out of the panel into any drop target";
            info->Author = L"Roman Kharitonov";
        }
    };

    struct StartupCall
    {
        const PluginStartupInfo *info;
        void run() const
        {
            if (info == nullptr) {
                burlak::plugin::composition().reset();
            } else {
                burlak::plugin::composition().setStartupInfo(*info);
            }
        }
    };

    struct PluginInfoCall
    {
        PluginInfo *info;
        void run() const
        {
            info->StructSize = sizeof(*info);
            info->Flags = PF_NONE;
        }
    };

    struct ConsoleInputCall
    {
        ProcessConsoleInputInfo *info{};
        intptr_t result{};
        void run()
        {
            if (info == nullptr || info->Rec.EventType != MOUSE_EVENT) {
                return;
            }
            const auto verdict = burlak::plugin::composition().feed(toCore(info->Rec.Event.MouseEvent));
            if (verdict.action == burlak::core::VerdictAction::Hold) {
                result = 1;
                return;
            }
            if (verdict.action == burlak::core::VerdictAction::Replace) {
                fromCore(*verdict.replacement, info->Rec.Event.MouseEvent);
                result = 2;
            }
        }
    };

    struct SynchroCall
    {
        const ProcessSynchroEventInfo *info;
        void run() const
        {
            if (info != nullptr && info->Event == SE_COMMONSYNCHRO) {
                burlak::plugin::composition().synchro();
            }
        }
    };

} // namespace

void WINAPI GetGlobalInfoW(GlobalInfo *info)
{
    GlobalInfoCall call{info};
    burlak::plugin::firewall([&call] { call.run(); });
}

void WINAPI SetStartupInfoW(const PluginStartupInfo *info)
{
    StartupCall call{info};
    burlak::plugin::firewall([&call] { call.run(); });
}

void WINAPI GetPluginInfoW(PluginInfo *info)
{
    PluginInfoCall call{info};
    burlak::plugin::firewall([&call] { call.run(); });
}

// Renewal identifies a Far plugin by OpenW even though Burlak has no menu item, prefix, or object to open.
HANDLE WINAPI OpenW(const OpenInfo *)
{
    burlak::plugin::firewall([] {});
    return nullptr;
}

intptr_t WINAPI ProcessConsoleInputW(ProcessConsoleInputInfo *info)
{
    ConsoleInputCall call{info};
    burlak::plugin::firewall([&call] { call.run(); });
    return call.result;
}

intptr_t WINAPI ProcessSynchroEventW(const ProcessSynchroEventInfo *info)
{
    SynchroCall call{info};
    burlak::plugin::firewall([&call] { call.run(); });
    return 0;
}

void WINAPI ExitFARW(const ExitInfo *)
{
    burlak::plugin::firewall([] { burlak::plugin::composition().stop(); });
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, void *)
{
    return TRUE;
}
