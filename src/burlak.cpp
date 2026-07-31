// Burlak -- drag files out of Far Manager's panel into any Windows drop target.
//
// Named after the barge haulers who dragged boats along the riverbank: this
// plugin does the dragging, and the name says nothing about direction, so it
// still fits if dropping *into* Far is ever added.
//
// The awkward part is that Far is a console application: measured from inside,
// with WH_GETMESSAGE and WH_CALLWNDPROC both installed on Far's own thread,
// mouse messages arrive exactly zero times while the console input path
// delivers thousands. Mouse input goes to the console input buffer and never
// reaches the window layer, and the console window refuses to be subclassed
// (SetWindowLongPtr fails with ERROR_ACCESS_DENIED). So DoDragDrop, which
// lives on a modal message loop, cannot run on Far's main thread.
//
// The way through -- taken from karbazol/far-drag-n-drop-plugin, which solved
// this years ago -- is to re-found the gesture on a thread that *can* own it:
//
//   1. Far's main thread spots the drag in ProcessConsoleInputW.
//   2. It synthesises a button release, ending the console's idea of the
//      gesture.
//   3. A tool window, owned by a thread of our own with a normal message pump,
//      is raised over Far and takes the capture.
//   4. A button press is synthesised. It lands on the tool window, so that
//      thread now legitimately owns the mouse.
//   5. That thread runs the drag and pumps its own messages.
//
// The tool window is layered at alpha 1: present for hit-testing, invisible in
// practice, so nothing flashes over the panel.

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <plugin.hpp>

#include "version.h"

#include <string>
#include <vector>

static struct PluginStartupInfo Info;

// {6F3D1A54-7C2E-4B8A-9E31-0A5C4D2F8B17}
static const GUID PluginGuid =
    { 0x6f3d1a54, 0x7c2e, 0x4b8a, { 0x9e, 0x31, 0x0a, 0x5c, 0x4d, 0x2f, 0x8b, 0x17 } };
// {C21B5E90-4A76-4D53-8F0C-6B93E7A14D22}
static const GUID MenuGuid =
    { 0xc21b5e90, 0x4a76, 0x4d53, { 0x8f, 0x0c, 0x6b, 0x93, 0xe7, 0xa1, 0x4d, 0x22 } };

namespace {

constexpr UINT WM_PREPARE_DRAG = WM_USER + 0x101;
constexpr int  DRAG_THRESHOLD_CELLS = 3;
constexpr wchar_t TOOL_CLASS[] = L"BurlakToolWindow";

// ---------------------------------------------------------------- shared ---

// Filled by the main thread, read by the window thread while the main thread
// blocks inside SendMessage -- so no lock is needed.
std::vector<std::wstring> g_paths;

IDataObject* g_data = nullptr;
HWND g_tool = nullptr;
HANDLE g_thread = nullptr;
DWORD g_threadId = 0;
bool g_dragActive = false;

bool LeftButtonDown()
{
    return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
}

// ------------------------------------------------------------ data object ---

// Let the shell build the data object: the target then sees the full set of
// shell formats, exactly as if the files had come from Explorer, rather than a
// bare CF_HDROP we assembled by hand.
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

// ------------------------------------------------------------ tool window ---

void HideTool()
{
    if (g_tool)
        ShowWindow(g_tool, SW_HIDE);
}

// Cover Far's window so the synthesised press cannot miss us.
bool ShowTool()
{
    const HWND far_wnd = GetConsoleWindow();
    RECT r{};
    if (far_wnd && GetWindowRect(far_wnd, &r)) {
        SetWindowPos(g_tool, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
        SetWindowPos(g_tool, HWND_TOP, 0, 0, GetSystemMetrics(SM_CXSCREEN),
                     GetSystemMetrics(SM_CYSCREEN), SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    return IsWindowVisible(g_tool) != 0;
}

void RunDrag()
{
    if (!g_data || g_dragActive)
        return;

    g_dragActive = true;

    // A null IDropSource asks the shell for its default one (Vista+), which is
    // also what gives us the standard drag image for free.
    DWORD effect = 0;
    SHDoDragDrop(g_tool, g_data, nullptr,
                 DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);

    g_dragActive = false;
    ReleaseCapture();
    HideTool();

    if (g_data) {
        g_data->Release();
        g_data = nullptr;
    }
}

LRESULT CALLBACK ToolProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PREPARE_DRAG: {
        if (g_data) {
            g_data->Release();
            g_data = nullptr;
        }
        g_data = MakeDataObject(g_paths);
        if (!g_data)
            return 0;

        if (!ShowTool()) {
            g_data->Release();
            g_data = nullptr;
            return 0;
        }

        SetCapture(hwnd);

        // Re-press on our own window: this is what hands mouse ownership to
        // this thread. Far's main thread has already released the real one.
        if (!LeftButtonDown())
            mouse_event(MOUSEEVENTF_MOVE | MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);

        return 1;
    }

    case WM_LBUTTONDOWN:
        // Arrives from the synthesised press above; the drag runs here, on the
        // thread that owns both the window and the capture.
        RunDrag();
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
    if (g_tool) {
        // Present for hit-testing, invisible to the eye.
        SetLayeredWindowAttributes(g_tool, 0, 1, LWA_ALPHA);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

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

// ------------------------------------------------------- gesture, panel ----

struct Gesture {
    bool armed = false;
    int anchorX = 0;
    int anchorY = 0;

    // Returns true once, when the pointer has travelled far enough with the
    // button held. Everything else leaves Far's own handling alone.
    bool feed(const MOUSE_EVENT_RECORD& m)
    {
        const bool left = (m.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
        if (!left) {
            armed = false;
            return false;
        }
        if (!(m.dwEventFlags & MOUSE_MOVED)) {
            armed = true;
            anchorX = m.dwMousePosition.X;
            anchorY = m.dwMousePosition.Y;
            return false;
        }
        if (!armed)
            return false;

        const int dx = abs(m.dwMousePosition.X - anchorX);
        const int dy = abs(m.dwMousePosition.Y - anchorY);
        if (dx < DRAG_THRESHOLD_CELLS && dy < DRAG_THRESHOLD_CELLS)
            return false;

        armed = false;
        return true;
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

// Selected items, or the one under the cursor when nothing is marked -- which
// is what FCTL_GETSELECTEDPANELITEM already returns.
std::vector<std::wstring> SelectedPaths()
{
    std::vector<std::wstring> paths;

    PanelInfo pi{};
    pi.StructSize = sizeof(pi);
    if (!Info.PanelControl(PANEL_ACTIVE, FCTL_GETPANELINFO, 0, &pi))
        return paths;

    // Plugin panels (archives, FTP) have no path the shell could resolve.
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

bool BeginDrag()
{
    g_paths = SelectedPaths();
    if (g_paths.empty())
        return false;

    StartThread();
    if (!g_tool)
        return false;

    // End the console's gesture before the tool window re-founds it.
    if (LeftButtonDown())
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);

    // Runs on the tool thread; returns as soon as the window is up and armed,
    // so Far's main thread is not held for the duration of the drag.
    return SendMessageW(g_tool, WM_PREPARE_DRAG, 0, 0) != 0;
}

} // namespace

void WINAPI GetGlobalInfoW(struct GlobalInfo* gi)
{
    gi->StructSize = sizeof(*gi);
    gi->MinFarVersion = FARMANAGERVERSION;
    gi->Version = MAKEFARVERSION(BURLAK_VERSION_MAJOR, BURLAK_VERSION_MINOR, BURLAK_VERSION_PATCH, 0, VS_RELEASE);
    gi->Guid = PluginGuid;
    gi->Title = L"Burlak";
    gi->Description = L"Drag files out of the panel into any drop target";
    gi->Author = L"Roma Kharitonov";
}

void WINAPI SetStartupInfoW(const struct PluginStartupInfo* psi)
{
    Info = *psi;
}

void WINAPI GetPluginInfoW(struct PluginInfo* pi)
{
    pi->StructSize = sizeof(*pi);
    pi->Flags = PF_NONE;

    static const wchar_t* names[] = { L"Burlak" };
    static const GUID guids[] = { MenuGuid };
    pi->PluginMenu.Guids = guids;
    pi->PluginMenu.Strings = names;
    pi->PluginMenu.Count = ARRAYSIZE(names);
}

HANDLE WINAPI OpenW(const struct OpenInfo*)
{
    // FMSG_ALLINONE takes ONE newline-separated string cast to
    // const wchar_t* const*; the trailing lines are the buttons.
    static const wchar_t text[] =
        L"Burlak " BURLAK_VERSION_STRING L"\n"
        L"Hold the left button on a panel item and\n"
        L"move a few cells to drag it out of Far.\n"
        L"OK";
    Info.Message(&PluginGuid, &MenuGuid, FMSG_LEFTALIGN | FMSG_ALLINONE, nullptr,
                 reinterpret_cast<const wchar_t* const*>(text), 0, 1);
    return nullptr;
}

intptr_t WINAPI ProcessConsoleInputW(struct ProcessConsoleInputInfo* info)
{
    if (!info || info->Rec.EventType != MOUSE_EVENT || g_dragActive)
        return 0;

    if (!g_gesture.feed(info->Rec.Event.MouseEvent))
        return 0;

    if (!BeginDrag())
        return 0;

    return 1;  // swallow it, or Far starts its own internal drag as well
}

void WINAPI ExitFARW(const struct ExitInfo*)
{
    StopThread();
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE;
}
