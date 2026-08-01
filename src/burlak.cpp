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
constexpr int  DRAG_THRESHOLD_CELLS = 3;
constexpr wchar_t TOOL_CLASS[] = L"BurlakToolWindow";

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

        if (!LeftButtonDown())
            mouse_event(MOUSEEVENTF_MOVE | MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);

        return 1;
    }

    case WM_LBUTTONDOWN:
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
    if (g_tool)
        SetLayeredWindowAttributes(g_tool, 0, 1, LWA_ALPHA);

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

struct Gesture {
    bool armed = false;
    int anchorX = 0;
    int anchorY = 0;

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

bool BeginDrag()
{
    g_paths = SelectedPaths();
    if (g_paths.empty())
        return false;

    StartThread();
    if (!g_tool)
        return false;

    if (LeftButtonDown())
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);

    return SendMessageW(g_tool, WM_PREPARE_DRAG, 0, 0) != 0;
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

intptr_t WINAPI ProcessConsoleInputW(struct ProcessConsoleInputInfo* info)
{
    if (!info || info->Rec.EventType != MOUSE_EVENT || g_dragActive)
        return 0;

    if (!g_gesture.feed(info->Rec.Event.MouseEvent))
        return 0;

    if (!BeginDrag())
        return 0;

    return 1;
}

void WINAPI ExitFARW(const struct ExitInfo*)
{
    StopThread();
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE;
}
