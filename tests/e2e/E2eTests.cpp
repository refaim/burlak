#include "../Desktop.hpp"
#include "adapters/shell/Shell.hpp"
#include "core/Policies.hpp"
#include "drag/DragSource.hpp"

#include <doctest/doctest.h>

#include <shellapi.h>
#include <shlobj.h>
#include <windows.h>

#include <plugin.hpp>

#include <bit>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace
{

    class Module
    {
      public:
        explicit Module(const wchar_t *path) : value_{LoadLibraryW(path)}
        {
        }
        ~Module()
        {
            if (value_ != nullptr) {
                FreeLibrary(value_);
            }
        }
        Module(const Module &) = delete;
        Module &operator=(const Module &) = delete;
        [[nodiscard]] HMODULE get() const
        {
            return value_;
        }

      private:
        HMODULE value_{};
    };

    template <class Function> Function load(HMODULE module, const char *name)
    {
        return std::bit_cast<Function>(GetProcAddress(module, name));
    }

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
        if (command == ACTL_SYNCHRO) {
            return 1;
        }
        auto &window = *static_cast<WindowInfo *>(parameter);
        window.Type = WTYPE_PANELS;
        return 1;
    }

    class ReceivingTarget final : public IDropTarget
    {
      public:
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **object) override
        {
            if (object == nullptr) {
                return E_POINTER;
            }
            *object = nullptr;
            if (id != IID_IUnknown && id != IID_IDropTarget) {
                return E_NOINTERFACE;
            }
            *object = static_cast<IDropTarget *>(this);
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
        HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *, DWORD, POINTL, DWORD *effect) override
        {
            *effect = DROPEFFECT_COPY;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD *effect) override
        {
            *effect = DROPEFFECT_COPY;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE DragLeave() override
        {
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE Drop(IDataObject *data, DWORD, POINTL, DWORD *effect) override
        {
            FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            STGMEDIUM medium{};
            if (SUCCEEDED(data->GetData(&format, &medium))) {
                const auto drop = static_cast<HDROP>(GlobalLock(medium.hGlobal));
                if (drop != nullptr) {
                    wchar_t path[MAX_PATH]{};
                    received_ = DragQueryFileW(drop, 0, path, MAX_PATH) > 0;
                    GlobalUnlock(medium.hGlobal);
                }
                ReleaseStgMedium(&medium);
            }
            *effect = DROPEFFECT_COPY;
            return S_OK;
        }
        [[nodiscard]] bool received() const
        {
            return received_;
        }

      private:
        ULONG references_{1};
        bool received_{};
    };

    LRESULT CALLBACK targetProcedure(HWND window, UINT message, WPARAM word, LPARAM number)
    {
        return DefWindowProcW(window, message, word, number);
    }

    HWND targetWindow()
    {
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = targetProcedure;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"BurlakE2eDropTarget";
        static_cast<void>(RegisterClassW(&windowClass));
        const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        return CreateWindowExW(WS_EX_TOPMOST, windowClass.lpszClassName, L"", WS_POPUP | WS_VISIBLE, x, y, width,
                               height, nullptr, nullptr, windowClass.hInstance, nullptr);
    }

    class WindowGuard
    {
      public:
        WindowGuard() : value_{targetWindow()}
        {
        }
        ~WindowGuard()
        {
            if (value_ != nullptr) {
                DestroyWindow(value_);
            }
        }
        WindowGuard(const WindowGuard &) = delete;
        WindowGuard &operator=(const WindowGuard &) = delete;
        [[nodiscard]] HWND get() const
        {
            return value_;
        }

      private:
        HWND value_{};
    };

    class CursorGuard
    {
      public:
        CursorGuard() : captured_{GetCursorPos(&position_) != FALSE}
        {
        }
        ~CursorGuard()
        {
            if (captured_) {
                static_cast<void>(SetCursorPos(position_.x, position_.y));
            }
        }
        CursorGuard(const CursorGuard &) = delete;
        CursorGuard &operator=(const CursorGuard &) = delete;
        [[nodiscard]] bool captured() const
        {
            return captured_;
        }

      private:
        POINT position_{};
        bool captured_{};
    };

    class OleGuard
    {
      public:
        OleGuard() : status_{OleInitialize(nullptr)}
        {
        }
        ~OleGuard()
        {
            if (SUCCEEDED(status_)) {
                OleUninitialize();
            }
        }
        OleGuard(const OleGuard &) = delete;
        OleGuard &operator=(const OleGuard &) = delete;
        [[nodiscard]] HRESULT status() const
        {
            return status_;
        }

      private:
        HRESULT status_{};
    };

    class RegistrationGuard
    {
      public:
        RegistrationGuard(HWND window, IDropTarget &target)
            : window_{window}, status_{RegisterDragDrop(window, &target)}
        {
        }
        ~RegistrationGuard()
        {
            if (status_ == S_OK) {
                static_cast<void>(RevokeDragDrop(window_));
            }
        }
        RegistrationGuard(const RegistrationGuard &) = delete;
        RegistrationGuard &operator=(const RegistrationGuard &) = delete;
        [[nodiscard]] HRESULT status() const
        {
            return status_;
        }

      private:
        HWND window_{};
        HRESULT status_{};
    };

    class FileGuard
    {
      public:
        explicit FileGuard(std::filesystem::path path) : path_{std::move(path)}
        {
            std::ofstream stream{path_};
        }
        ~FileGuard()
        {
            std::error_code ignored;
            static_cast<void>(std::filesystem::remove(path_, ignored));
        }
        FileGuard(const FileGuard &) = delete;
        FileGuard &operator=(const FileGuard &) = delete;
        [[nodiscard]] const std::filesystem::path &path() const
        {
            return path_;
        }

      private:
        std::filesystem::path path_;
    };

    bool ownsDragPoint(HWND window)
    {
        RECT rect{};
        if (!GetWindowRect(window, &rect)) {
            return false;
        }
        const POINT requested{rect.left + 10, rect.top + 10};
        POINT positioned{};
        return SetCursorPos(requested.x, requested.y) != FALSE && GetCursorPos(&positioned) != FALSE &&
               positioned.x == requested.x && positioned.y == requested.y && WindowFromPoint(positioned) == window &&
               (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0 && (GetAsyncKeyState(VK_RBUTTON) & 0x8000) == 0;
    }

    bool controlledDragEnvironment()
    {
        const bool visible = burlak::tests::desktopAvailable(
            "SKIP: real OLE drag requires a visible window station with cursor access\n");
        if (!visible) {
            return false;
        }
        CursorGuard cursor;
        WindowGuard window;
        const bool controlled = visible && cursor.captured() && window.get() != nullptr && ownsDragPoint(window.get());
        if (!controlled) {
            std::fputs("SKIP: real OLE drag requires an owned cursor point with both mouse buttons up\n", stderr);
        }
        return controlled;
    }

} // namespace

TEST_SUITE("e2e")
{
    TEST_CASE("the built DLL loads and all seven exports execute through Far-shaped stubs")
    {
        Module module{BURLAK_DLL_PATH};
        REQUIRE(module.get() != nullptr);

        const auto global = load<decltype(&GetGlobalInfoW)>(module.get(), "GetGlobalInfoW");
        const auto startup = load<decltype(&SetStartupInfoW)>(module.get(), "SetStartupInfoW");
        const auto plugin = load<decltype(&GetPluginInfoW)>(module.get(), "GetPluginInfoW");
        const auto open = load<decltype(&OpenW)>(module.get(), "OpenW");
        const auto input = load<decltype(&ProcessConsoleInputW)>(module.get(), "ProcessConsoleInputW");
        const auto synchro = load<decltype(&ProcessSynchroEventW)>(module.get(), "ProcessSynchroEventW");
        const auto exit = load<decltype(&ExitFARW)>(module.get(), "ExitFARW");
        REQUIRE(global != nullptr);
        REQUIRE(startup != nullptr);
        REQUIRE(plugin != nullptr);
        REQUIRE(open != nullptr);
        REQUIRE(input != nullptr);
        REQUIRE(synchro != nullptr);
        REQUIRE(exit != nullptr);

        GlobalInfo globalInfo{};
        global(&globalInfo);
        CHECK(globalInfo.Version.Major == 1);
        CHECK(globalInfo.Version.Minor == 3);
        PluginStartupInfo startupInfo{};
        startupInfo.StructSize = sizeof(startupInfo);
        startupInfo.PanelControl = panelControl;
        startupInfo.AdvControl = advControl;
        startup(&startupInfo);
        PluginInfo pluginInfo{};
        plugin(&pluginInfo);
        CHECK(pluginInfo.StructSize == sizeof(pluginInfo));
        CHECK(open(nullptr) == nullptr);
        CHECK(input(nullptr) == 0);
        ProcessSynchroEventInfo event{};
        event.Event = SE_COMMONSYNCHRO;
        CHECK(synchro(&event) == 0);
        exit(nullptr);
    }

    TEST_CASE("a real OLE drag transfers a shell data object into the test drop target" *
              doctest::skip(!controlledDragEnvironment()))
    {
        OleGuard ole;
        REQUIRE(SUCCEEDED(ole.status()));
        FileGuard file{std::filesystem::temp_directory_path() / L"burlak-e2e-drag.txt"};
        const std::vector<std::wstring> paths{file.path().wstring()};
        auto data = burlak::adapters::shell::makeDataObject(paths);
        REQUIRE(data.has_value());

        WindowGuard window;
        REQUIRE(window.get() != nullptr);
        ReceivingTarget target;
        RegistrationGuard registration{window.get(), target};
        REQUIRE(registration.status() == S_OK);
        CursorGuard cursor;
        REQUIRE(cursor.captured());
        if (!ownsDragPoint(window.get())) {
            std::fputs("SKIP: real OLE drag lost its owned cursor point or a mouse button is down\n", stderr);
            return;
        }

        burlak::core::ReleasePolicy policy;
        burlak::drag::DragSource source{policy, burlak::core::Button::Left};
        const auto dragResult = burlak::adapters::shell::runDrag(window.get(), *data->data.Get(), source,
                                                                 burlak::adapters::shell::systemShellCalls());
        REQUIRE(dragResult.status == DRAGDROP_S_DROP);
        CHECK(target.received());
    }
}
