#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <plugin.hpp>

#include "version.h"

#include <string>
#include <vector>

static struct PluginStartupInfo Info;

static const GUID PluginGuid =
    { 0x130a60a7, 0x8d79, 0x483c, { 0x93, 0xf0, 0x8d, 0xca, 0xb2, 0x70, 0x22, 0xc9 } };

namespace {

constexpr UINT WM_PREPARE_DRAG = WM_USER + 0x101;
constexpr UINT WM_START_DRAG = WM_USER + 0x102;
constexpr UINT WM_ABORT_DRAG = WM_USER + 0x103;
constexpr UINT_PTR ARM_TIMER = 1;
constexpr UINT ARM_TIMEOUT_MS = 1000;
constexpr int  DRAG_THRESHOLD_CELLS = 3;
constexpr wchar_t TOOL_CLASS[] = L"BurlakToolWindow";

std::vector<std::wstring> g_paths;

IDataObject* g_data = nullptr;
HWND g_tool = nullptr;
HANDLE g_thread = nullptr;
DWORD g_threadId = 0;
bool g_dragActive = false;
int g_button = VK_LBUTTON;  // the mouse button the drag rides on

bool ButtonDown(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

IDataObject* MakeDataObject(const std::vector<std::wstring>& paths)
{
    std::vector<PIDLIST_ABSOLUTE> pidls;
    pidls.reserve(paths.size());

    for (const std::wstring& p : paths) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr)) && pidl)
            pidls.push_back(pidl);
    }
    if (pidls.empty())
        return nullptr;

    IShellItemArray* array = nullptr;
    IDataObject* data = nullptr;
    if (SUCCEEDED(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()),
                                                    const_cast<PCIDLIST_ABSOLUTE*>(pidls.data()),
                                                    &array)) && array) {
        array->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
        array->Release();
    }

    for (PIDLIST_ABSOLUTE pidl : pidls)
        CoTaskMemFree(pidl);

    return data;
}

void HideTool()
{
    if (g_tool)
        ShowWindow(g_tool, SW_HIDE);
}

void DropData()
{
    if (g_data) {
        g_data->Release();
        g_data = nullptr;
    }
}

bool Covers(HWND wnd, POINT pt, RECT* r)
{
    return wnd && IsWindowVisible(wnd) && GetWindowRect(wnd, r) && PtInRect(r, pt);
}

// The window Far is drawn in, which the tool window has to cover.
//
// A window on a background thread only gets mouse input while the cursor is
// over a visible part of it, so the tool window must be under the cursor when
// the synthetic click lands. Covering the terminal for the whole drag also
// stops it from taking a drop of its own files.
//
// A real console (conhost, OpenConsole in its own window) is a visible window
// with a proper rect. Under a ConPTY host (Windows Terminal, VS Code's
// terminal) GetConsoleWindow() is a PseudoConsoleWindow that reports itself
// visible but 0x0, so the cursor can't be in it; ConEmu keeps a real console
// window and hides it, so it fails the visibility test instead. Then the
// terminal is the foreground window: it has focus, or Far wouldn't be seeing
// these mouse events.
HWND HostWindow(RECT* r)
{
    POINT pt{};
    if (!GetCursorPos(&pt))
        return nullptr;

    const HWND console = GetConsoleWindow();
    if (Covers(console, pt, r))
        return console;

    const HWND foreground = GetForegroundWindow();
    if (Covers(foreground, pt, r))
        return foreground;

    return nullptr;
}

bool ShowTool(HWND host, const RECT& r)
{
    // Directly above the host: in the topmost band only when the terminal is
    // there itself (Windows Terminal's alwaysOnTop), so the tool window never
    // shadows a pinned window a drop may be aimed at. HWND_NOTOPMOST only
    // demotes a window, it does not raise one, hence the separate step.
    const bool topmost = (GetWindowLongPtrW(host, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    if (!topmost && (GetWindowLongPtrW(g_tool, GWL_EXSTYLE) & WS_EX_TOPMOST))
        SetWindowPos(g_tool, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(g_tool, topmost ? HWND_TOPMOST : HWND_TOP, r.left, r.top,
                 r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return IsWindowVisible(g_tool) != 0;
}

void RunDrag()
{
    if (!g_data || g_dragActive)
        return;

    g_dragActive = true;

    DWORD effect = 0;
    SHDoDragDrop(g_tool, g_data, nullptr,
                 DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);

    g_dragActive = false;
    ReleaseCapture();
    HideTool();
    DropData();
}

// The synthetic click never arrived. Without this the tool window would stay
// up over the terminal, holding capture and the stale paths, until the user's
// next click in it started a drag nobody asked for.
void Disarm(HWND hwnd)
{
    KillTimer(hwnd, ARM_TIMER);
    if (g_dragActive)
        return;
    ReleaseCapture();
    HideTool();
    DropData();
}

LRESULT CALLBACK ToolProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PREPARE_DRAG:
        DropData();
        g_data = MakeDataObject(g_paths);
        return g_data != nullptr;

    case WM_START_DRAG: {
        // Measured here, on the thread that positions the tool window and
        // right before the click is injected: the user is mid-drag, and the
        // cursor may have moved on since BeginDrag looked.
        RECT r{};
        const HWND host = HostWindow(&r);
        if (!g_data || !host || !ShowTool(host, r)) {
            DropData();
            return 0;
        }
        SetCapture(hwnd);
        SetTimer(hwnd, ARM_TIMER, ARM_TIMEOUT_MS, nullptr);
        // BeginDrag has just released the button; press it again, now over the
        // tool window. Input is serialised, so this lands after that release
        // even if GetAsyncKeyState hasn't caught up with it yet.
        mouse_event(MOUSEEVENTF_MOVE | (g_button == VK_LBUTTON ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_RIGHTDOWN),
                    0, 0, 0, 0);
        return 1;
    }

    case WM_ABORT_DRAG:
        DropData();
        return 0;

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        KillTimer(hwnd, ARM_TIMER);
        RunDrag();
        return 0;

    case WM_TIMER:
        if (wp != ARM_TIMER)
            break;
        Disarm(hwnd);
        return 0;

    case WM_DESTROY:
        HideTool();
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

DWORD WINAPI ToolThread(LPVOID)
{
    OleInitialize(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ToolProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = TOOL_CLASS;
    RegisterClassExW(&wc);

    g_tool = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             TOOL_CLASS, nullptr, WS_POPUP,
                             0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    if (g_tool)
        SetLayeredWindowAttributes(g_tool, 0, 1, LWA_ALPHA);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DropData();
    if (g_tool) {
        DestroyWindow(g_tool);
        g_tool = nullptr;
    }
    OleUninitialize();
    return 0;
}

void StartThread()
{
    if (g_thread)
        return;
    g_thread = CreateThread(nullptr, 0, ToolThread, nullptr, 0, &g_threadId);
    for (int i = 0; i < 200 && !g_tool; ++i)
        Sleep(5);
}

void StopThread()
{
    if (g_threadId)
        PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
    if (g_thread) {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}

// Whether the point is on the panel's item rows.
//
// PanelRect is the whole panel. Its first two rows (frame, column titles)
// and last three (status line, frame) are where Far, for as long as any
// button is held, scrolls the cursor in a loop of its own that reads the
// console directly, so nothing the gesture does reaches the panel from
// there; the right frame column holds the scrollbar, which Far drags the
// same way, and the left one goes with it for the simpler rule.
// With column titles or the status line turned off an item row or two falls
// into the trim as well, and a drag can't start from those; that is all.
//
// A panel without real names, an archive or an FTP one, has nothing to
// drag and is left to Far entirely, so Far's own panel-to-panel mouse drag
// keeps working there.
bool InsidePanel(HANDLE panel, COORD pt)
{
    PanelInfo pi{};
    pi.StructSize = sizeof(pi);
    if (!Info.PanelControl(panel, FCTL_GETPANELINFO, 0, &pi))
        return false;
    if (!(pi.Flags & PFLAGS_VISIBLE) || !(pi.Flags & PFLAGS_REALNAMES))
        return false;
    const RECT& r = pi.PanelRect;
    return pt.X > r.left && pt.X < r.right && pt.Y >= r.top + 2 && pt.Y <= r.bottom - 3;
}

// Whether a press here lands on a panel's items, the only place the gesture
// means anything: Far puts the cursor on the item under the mouse, and that item,
// or the selection around it, is what BeginDrag reads back. In the editor,
// a dialog or the command line a held button is a selection or a window
// being dragged, and Far must keep seeing every move. Both panels count,
// as a press on the passive one makes it active before the drag starts.
bool OnPanel(COORD pt)
{
    WindowInfo wi{};
    wi.StructSize = sizeof(wi);
    wi.Pos = -1;
    if (!Info.AdvControl(&PluginGuid, ACTL_GETWINDOWINFO, 0, &wi) || wi.Type != WTYPE_PANELS)
        return false;
    return InsidePanel(PANEL_ACTIVE, pt) || InsidePanel(PANEL_PASSIVE, pt);
}

bool BeginDrag(int button);

// What becomes of a mouse event: Far gets it, Far never sees it, or Far gets
// the record it has been rewritten into.
enum class Verdict { Pass, Hold, Replace };

// The left gesture leaves the press to Far, which puts the panel cursor on
// the item under the mouse, and holds back the moves that follow: Far walks
// the cursor after a held button, and a drag that started a few cells later
// would take whatever the cursor is on by then, not what was pressed.
//
// The right gesture can't leave the press to Far, which reads a right press
// on an item as "toggle its selection", or with RightClickSelect off as
// "context menu when the button comes up": one changes what gets dragged,
// the other pops a menu under the drag. So the press is held back as well.
// Released short of the threshold it was a click, and Far gets the press
// then, unchanged, just later.
//
// Crossing the threshold, the move is rewritten into the one record that
// leaves the panel ready for the drag, and the drag starts on the synchro
// event: Far delivers that before it reads any more input, so by then the
// record has been dealt with.
struct Gesture {
    enum class Phase { Idle, Armed, Starting, Spent };

    Phase phase = Phase::Idle;
    int button = VK_LBUTTON;
    MOUSE_EVENT_RECORD press{};

    Verdict feed(MOUSE_EVENT_RECORD& m)
    {
        if (m.dwEventFlags & (MOUSE_WHEELED | MOUSE_HWHEELED))
            return Verdict::Pass;

        const bool moved = (m.dwEventFlags & MOUSE_MOVED) != 0;
        const bool left = (m.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
        const bool right = (m.dwButtonState & RIGHTMOST_BUTTON_PRESSED) != 0;

        if (phase == Phase::Idle) {
            if (moved || !(left || right))
                return Verdict::Pass;
            return arm(left ? VK_LBUTTON : VK_RBUTTON, m);
        }

        if (!(button == VK_LBUTTON ? left : right)) {
            // The button is up. A right press held back this far was a click;
            // anything else starts over as if nothing had been in progress.
            const bool click = phase == Phase::Armed && button == VK_RBUTTON;
            phase = Phase::Idle;
            if (!click)
                return feed(m);
            m = press;
            return Verdict::Replace;
        }

        if (!moved)
            return button == VK_LBUTTON ? arm(VK_LBUTTON, m) : Verdict::Hold;

        if (phase != Phase::Armed)
            return Verdict::Hold;

        const int dx = abs(m.dwMousePosition.X - press.dwMousePosition.X);
        const int dy = abs(m.dwMousePosition.Y - press.dwMousePosition.Y);
        if (dx < DRAG_THRESHOLD_CELLS && dy < DRAG_THRESHOLD_CELLS)
            return Verdict::Hold;

        m = press;
        if (button == VK_LBUTTON) {
            // Far took the press as the start of its own panel-to-panel drag.
            // A release here, in the pressed panel, ends that quietly; the
            // release the drag really starts with lands wherever the mouse is
            // by then, and inside the other panel Far would take it for a
            // drop and start copying.
            m.dwButtonState = 0;
            m.dwEventFlags = 0;
        } else {
            // Far never saw the press. A move with the left button held puts
            // the cursor on the item and nothing more: no selection toggle,
            // no drag of Far's own, and none of the scrollbar and disk-menu
            // paths, which only a button event enters.
            m.dwButtonState = FROM_LEFT_1ST_BUTTON_PRESSED;
            m.dwEventFlags = MOUSE_MOVED;
        }
        phase = Phase::Starting;
        Info.AdvControl(&PluginGuid, ACTL_SYNCHRO, 0, nullptr);
        return Verdict::Replace;
    }

    // Far has dealt with the rewritten record: the cursor is on the pressed
    // item and nothing of Far's is in flight, so the drag can start.
    void synchro()
    {
        if (phase != Phase::Starting)
            return;
        phase = Phase::Spent;
        BeginDrag(button);
    }

    // The drag took over; the terminal sees nothing more of the gesture, not
    // even its release, so it must not wait for one.
    void reset()
    {
        phase = Phase::Idle;
    }

private:
    Verdict arm(int vk, const MOUSE_EVENT_RECORD& m)
    {
        if (!OnPanel(m.dwMousePosition)) {
            phase = Phase::Idle;
            return Verdict::Pass;
        }
        phase = Phase::Armed;
        button = vk;
        press = m;
        return vk == VK_LBUTTON ? Verdict::Pass : Verdict::Hold;
    }
};

Gesture g_gesture;

std::wstring PanelDirectory()
{
    const size_t size = Info.PanelControl(PANEL_ACTIVE, FCTL_GETPANELDIRECTORY, 0, nullptr);
    if (!size)
        return {};

    std::vector<char> buf(size);
    FarPanelDirectory* dir = reinterpret_cast<FarPanelDirectory*>(buf.data());
    dir->StructSize = sizeof(FarPanelDirectory);
    if (!Info.PanelControl(PANEL_ACTIVE, FCTL_GETPANELDIRECTORY, size, dir))
        return {};

    return dir->Name ? dir->Name : L"";
}

std::vector<std::wstring> SelectedPaths()
{
    std::vector<std::wstring> paths;

    PanelInfo pi{};
    pi.StructSize = sizeof(pi);
    if (!Info.PanelControl(PANEL_ACTIVE, FCTL_GETPANELINFO, 0, &pi))
        return paths;

    if (!(pi.Flags & PFLAGS_REALNAMES))
        return paths;

    std::wstring dir = PanelDirectory();
    if (dir.empty())
        return paths;
    if (dir.back() != L'\\')
        dir += L'\\';

    for (size_t i = 0; i < pi.SelectedItemsNumber; ++i) {
        const size_t size = Info.PanelControl(PANEL_ACTIVE, FCTL_GETSELECTEDPANELITEM, i, nullptr);
        if (!size)
            continue;

        std::vector<char> buf(size);
        FarGetPluginPanelItem item{ sizeof(FarGetPluginPanelItem), size,
                                    reinterpret_cast<PluginPanelItem*>(buf.data()) };
        if (!Info.PanelControl(PANEL_ACTIVE, FCTL_GETSELECTEDPANELITEM, i, &item))
            continue;

        const wchar_t* name = item.Item->FileName;
        if (!name || !wcscmp(name, L"..") || !wcscmp(name, L"."))
            continue;

        paths.push_back(dir + name);
    }
    return paths;
}

bool BeginDrag(int button)
{
    g_paths = SelectedPaths();
    if (g_paths.empty())
        return false;

    StartThread();
    if (!g_tool)
        return false;

    g_button = button;

    // The data object is the slow part (a shell lookup per path), so it is
    // built before the gate below: nothing may take time between the last
    // look at the cursor and the button being touched.
    if (!ButtonDown(button) || !SendMessageW(g_tool, WM_PREPARE_DRAG, 0, 0))
        return false;

    // The button is only touched once the drag is sure to start, so a gesture
    // that can't become one stays an ordinary click as far as Far and the
    // user are concerned. A button that is already up means the gesture is
    // over: pressing it again would leave the system believing it is held.
    RECT r{};
    if (!HostWindow(&r) || !ButtonDown(button)) {
        SendMessageW(g_tool, WM_ABORT_DRAG, 0, 0);
        return false;
    }

    // Release the button the user is holding, so the press synthesised over
    // the tool window is a fresh click there rather than a continuation of
    // the one the terminal saw.
    mouse_event(button == VK_LBUTTON ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0);
    return SendMessageW(g_tool, WM_START_DRAG, 0, 0) != 0;
}

}

void WINAPI GetGlobalInfoW(struct GlobalInfo* gi)
{
    gi->StructSize = sizeof(*gi);
    gi->MinFarVersion = MAKEFARVERSION(3, 0, 0, 2843, VS_RELEASE);
    gi->Version = MAKEFARVERSION(BURLAK_VERSION_MAJOR, BURLAK_VERSION_MINOR, BURLAK_VERSION_PATCH, 0, VS_RELEASE);
    gi->Guid = PluginGuid;
    gi->Title = L"Burlak";
    gi->Description = L"Drag files out of the panel into any drop target";
    gi->Author = L"Roman Kharitonov";
}

void WINAPI SetStartupInfoW(const struct PluginStartupInfo* psi)
{
    Info = *psi;
}

void WINAPI GetPluginInfoW(struct PluginInfo* pi)
{
    pi->StructSize = sizeof(*pi);
    pi->Flags = PF_NONE;
}

// Nothing to open: the plugin has no menu item or prefix, and a macro's
// Plugin.Call gets nothing back. The export exists for Renewal, the
// autoupdate plugin, which takes a DLL for a Far plugin only if it exports
// OpenW (OpenPlugin for Far 1.x); without it the update it has downloaded is
// discarded as "unable to determine new source path".
HANDLE WINAPI OpenW(const struct OpenInfo*)
{
    return nullptr;
}

intptr_t WINAPI ProcessConsoleInputW(struct ProcessConsoleInputInfo* info)
{
    if (!info || info->Rec.EventType != MOUSE_EVENT)
        return 0;
    if (g_dragActive) {
        g_gesture.reset();
        return 0;
    }

    switch (g_gesture.feed(info->Rec.Event.MouseEvent)) {
    case Verdict::Hold:
        return 1;
    case Verdict::Replace:
        return 2;
    case Verdict::Pass:
        break;
    }
    return 0;
}

intptr_t WINAPI ProcessSynchroEventW(const struct ProcessSynchroEventInfo* info)
{
    if (info && info->Event == SE_COMMONSYNCHRO)
        g_gesture.synchro();
    return 0;
}

void WINAPI ExitFARW(const struct ExitInfo*)
{
    StopThread();
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE;
}
