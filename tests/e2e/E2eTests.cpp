#include "../Desktop.hpp"
#include "adapters/shell/Shell.hpp"
#include "adapters/win/Screen.hpp"
#include "core/Policies.hpp"
#include "drag/DragSource.hpp"

#include <doctest/doctest.h>

#include <shellapi.h>
#include <shlobj.h>
#include <windows.h>

#include <plugin.hpp>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace
{

    std::atomic<int> synchroRequests{};
    bool pluginPanel{};
    const UUID pluginOwner{0x12345678, 0x1111, 0x2222, {1, 2, 3, 4, 5, 6, 7, 8}};
    GlobalInfo pluginGlobal{};
    std::wstring pluginModule{GETFILES_SUCCESS_PATH};
    const std::wstring pluginItem{L"plugin-e2e.txt"};

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
            info.PanelType = PTYPE_FILEPANEL;
            info.Flags = panel == PANEL_ACTIVE ? PFLAGS_VISIBLE | (pluginPanel ? PFLAGS_PLUGIN : PFLAGS_REALNAMES)
                                               : PFLAGS_VISIBLE | PFLAGS_REALNAMES;
            info.PanelRect = panel == PANEL_ACTIVE ? RECT{0, 0, 39, 24} : RECT{40, 0, 79, 24};
            if (pluginPanel && panel == PANEL_ACTIVE) {
                info.PluginHandle = reinterpret_cast<HANDLE>(55);
                info.OwnerGuid = pluginOwner;
                info.SelectedItemsNumber = 1;
                info.CurrentItem = 0;
            }
            return 1;
        }
        if (command == FCTL_GETSELECTEDPANELITEM && pluginPanel && panel == PANEL_ACTIVE) {
            const auto bytes = sizeof(PluginPanelItem) + (pluginItem.size() + 1) * sizeof(wchar_t);
            if (parameter == nullptr) {
                return static_cast<intptr_t>(bytes);
            }
            auto &request = *static_cast<FarGetPluginPanelItem *>(parameter);
            *request.Item = {};
            auto *name =
                reinterpret_cast<wchar_t *>(reinterpret_cast<std::byte *>(request.Item) + sizeof(PluginPanelItem));
            std::memcpy(name, pluginItem.c_str(), (pluginItem.size() + 1) * sizeof(wchar_t));
            request.Item->FileName = name;
            request.Item->FileSize = 7;
            request.Item->FileAttributes = FILE_ATTRIBUTE_HIDDEN;
            request.Item->Flags = PPIF_SELECTED;
            request.Item->UserData.Data = reinterpret_cast<void *>(29);
            return static_cast<intptr_t>(bytes);
        }
        if (command == FCTL_GETPANELDIRECTORY) {
            constexpr wchar_t directory[] = L"C:\\e2e";
            constexpr auto bytes = sizeof(FarPanelDirectory) + sizeof(directory);
            if (parameter == nullptr) {
                return bytes;
            }
            auto &request = *static_cast<FarPanelDirectory *>(parameter);
            auto *name =
                reinterpret_cast<wchar_t *>(reinterpret_cast<std::byte *>(parameter) + sizeof(FarPanelDirectory));
            std::memcpy(name, directory, sizeof(directory));
            request.Name = name;
            return bytes;
        }
        return 0;
    }

    intptr_t WINAPI advControl(const UUID *, ADVANCED_CONTROL_COMMANDS command, intptr_t, void *parameter)
    {
        if (command == ACTL_SYNCHRO) {
            ++synchroRequests;
            return 1;
        }
        auto &window = *static_cast<WindowInfo *>(parameter);
        window.Type = WTYPE_PANELS;
        return 1;
    }

    intptr_t WINAPI pluginsControl(HANDLE, FAR_PLUGINS_CONTROL_COMMANDS command, intptr_t, void *parameter)
    {
        if (command == PCTL_FINDPLUGIN) {
            const auto &requested = *static_cast<UUID *>(parameter);
            return std::memcmp(&requested, &pluginOwner, sizeof(pluginOwner)) == 0 ? 91 : 0;
        }
        if (parameter == nullptr) {
            return sizeof(FarGetPluginInformation);
        }
        auto &information = *static_cast<FarGetPluginInformation *>(parameter);
        information.ModuleName = pluginModule.c_str();
        information.GInfo = &pluginGlobal;
        return sizeof(FarGetPluginInformation);
    }

    class ReceivingTarget final : public IDropTarget
    {
      public:
        explicit ReceivingTarget(std::wstring expectedPlaceholder = {})
            : expectedPlaceholder_{std::move(expectedPlaceholder)}
        {
        }

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
        HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *data, DWORD, POINTL, DWORD *effect) override
        {
            entered_.store(true);
            const auto path = firstPath(data);
            placeholdersReady_.store(path && path->filename() == expectedPlaceholder_ &&
                                     std::filesystem::is_regular_file(*path) && std::filesystem::file_size(*path) == 0);
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
            const auto path = firstPath(data);
            received_.store(path.has_value());
            if (path) {
                extracted_.store(std::filesystem::exists(path->parent_path() / L"plugin-e2e.extracted"));
            }
            *effect = DROPEFFECT_COPY;
            return S_OK;
        }
        [[nodiscard]] bool received() const
        {
            return received_.load();
        }

        [[nodiscard]] bool entered() const
        {
            return entered_.load();
        }

        [[nodiscard]] bool placeholdersReady() const
        {
            return placeholdersReady_.load();
        }

        [[nodiscard]] bool extracted() const
        {
            return extracted_.load();
        }

      private:
        [[nodiscard]] static std::optional<std::filesystem::path> firstPath(IDataObject *data)
        {
            FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            STGMEDIUM medium{};
            if (data == nullptr || FAILED(data->GetData(&format, &medium))) {
                return std::nullopt;
            }
            std::optional<std::filesystem::path> result;
            const auto drop = static_cast<HDROP>(GlobalLock(medium.hGlobal));
            if (drop != nullptr) {
                wchar_t path[MAX_PATH]{};
                if (DragQueryFileW(drop, 0, path, MAX_PATH) > 0) {
                    result = path;
                }
                GlobalUnlock(medium.hGlobal);
            }
            ReleaseStgMedium(&medium);
            return result;
        }

        std::atomic<ULONG> references_{1};
        std::wstring expectedPlaceholder_;
        std::atomic<bool> received_{};
        std::atomic<bool> entered_{};
        std::atomic<bool> placeholdersReady_{};
        std::atomic<bool> extracted_{};
    };

    LRESULT CALLBACK targetProcedure(HWND window, UINT message, WPARAM word, LPARAM number)
    {
        return DefWindowProcW(window, message, word, number);
    }

    HWND targetWindow(int x, int y, int width, int height, bool topmost)
    {
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = targetProcedure;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"BurlakE2eDropTarget";
        static_cast<void>(RegisterClassW(&windowClass));
        return CreateWindowExW(topmost ? WS_EX_TOPMOST : 0, windowClass.lpszClassName, L"", WS_POPUP | WS_VISIBLE, x, y,
                               width, height, nullptr, nullptr, windowClass.hInstance, nullptr);
    }

    HWND targetWindow()
    {
        const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        return targetWindow(x, y, width, height, true);
    }

    class WindowGuard
    {
      public:
        WindowGuard() : value_{targetWindow()}
        {
        }
        WindowGuard(int x, int y, int width, int height, bool topmost)
            : value_{targetWindow(x, y, width, height, topmost)}
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

    class NoExtraction final : public burlak::core::IExtraction
    {
      public:
        [[nodiscard]] bool extract() override
        {
            return false;
        }

        void cleanup() override
        {
        }
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

    class MouseButtonGuard
    {
      public:
        void press()
        {
            mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
            down_ = true;
        }

        void release()
        {
            if (down_) {
                mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
                down_ = false;
            }
        }

        ~MouseButtonGuard()
        {
            release();
        }

        MouseButtonGuard() = default;
        MouseButtonGuard(const MouseButtonGuard &) = delete;
        MouseButtonGuard &operator=(const MouseButtonGuard &) = delete;

      private:
        bool down_{};
    };

    class PluginRuntimeGuard
    {
      public:
        explicit PluginRuntimeGuard(decltype(&ExitFARW) exit) : exit_{exit}
        {
        }

        ~PluginRuntimeGuard()
        {
            exit_(nullptr);
            pluginPanel = false;
        }

        PluginRuntimeGuard(const PluginRuntimeGuard &) = delete;
        PluginRuntimeGuard &operator=(const PluginRuntimeGuard &) = delete;

      private:
        decltype(&ExitFARW) exit_;
    };

    template <class Predicate> bool pumpUntil(Predicate predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!predicate() && std::chrono::steady_clock::now() < deadline) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
                static_cast<void>(TranslateMessage(&message));
                static_cast<void>(DispatchMessageW(&message));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        return predicate();
    }

    POINT inside(HWND window)
    {
        RECT rect{};
        static_cast<void>(GetWindowRect(window, &rect));
        return {rect.left + 10, rect.top + 10};
    }

    ProcessConsoleInputInfo farMouse(COORD cell, DWORD buttons, DWORD flags = 0)
    {
        ProcessConsoleInputInfo info{};
        info.StructSize = sizeof(info);
        info.Rec.EventType = MOUSE_EVENT;
        info.Rec.Event.MouseEvent.dwMousePosition = cell;
        info.Rec.Event.MouseEvent.dwButtonState = buttons;
        info.Rec.Event.MouseEvent.dwEventFlags = flags;
        return info;
    }

    PluginStartupInfo pluginStartup()
    {
        PluginStartupInfo info{};
        info.StructSize = sizeof(info);
        info.PanelControl = panelControl;
        info.AdvControl = advControl;
        info.PluginsControl = pluginsControl;
        return info;
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

    TEST_CASE("a plugin-panel drag advertises placeholders and extracts on the release synchro" *
              doctest::skip(!controlledDragEnvironment()))
    {
        OleGuard ole;
        REQUIRE(SUCCEEDED(ole.status()));
        Module module{BURLAK_DLL_PATH};
        REQUIRE(module.get() != nullptr);
        const auto startup = load<decltype(&SetStartupInfoW)>(module.get(), "SetStartupInfoW");
        const auto input = load<decltype(&ProcessConsoleInputW)>(module.get(), "ProcessConsoleInputW");
        const auto synchro = load<decltype(&ProcessSynchroEventW)>(module.get(), "ProcessSynchroEventW");
        const auto exit = load<decltype(&ExitFARW)>(module.get(), "ExitFARW");
        REQUIRE(startup != nullptr);
        REQUIRE(input != nullptr);
        REQUIRE(synchro != nullptr);
        REQUIRE(exit != nullptr);

        const int virtualX = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int virtualY = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int virtualWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int virtualHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        constexpr int windowWidth = 240;
        constexpr int windowHeight = 180;
        const std::array positions{
            POINT{virtualX + 20, virtualY + 20}, POINT{virtualX + virtualWidth - windowWidth - 20, virtualY + 20},
            POINT{virtualX + 20, virtualY + virtualHeight - windowHeight - 20},
            POINT{virtualX + virtualWidth - windowWidth - 20, virtualY + virtualHeight - windowHeight - 20}};
        RECT consoleRect{};
        const HWND console = GetConsoleWindow();
        const bool consoleVisible =
            console != nullptr && IsWindowVisible(console) != FALSE && GetWindowRect(console, &consoleRect) != FALSE;
        std::optional<POINT> sourcePosition;
        for (const auto position : positions) {
            const POINT cursor{position.x + 10, position.y + 10};
            if (!consoleVisible || PtInRect(&consoleRect, cursor) == FALSE) {
                sourcePosition = position;
                break;
            }
        }
        if (!sourcePosition) {
            std::fputs("SKIP: no cursor point outside the console is available for the plugin drag\n", stderr);
            return;
        }
        const auto targetPosition = positions.back().x == sourcePosition->x && positions.back().y == sourcePosition->y
                                        ? positions.front()
                                        : positions.back();

        WindowGuard source{sourcePosition->x, sourcePosition->y, windowWidth, windowHeight, false};
        WindowGuard targetWindowGuard{targetPosition.x, targetPosition.y, windowWidth, windowHeight, false};
        REQUIRE(source.get() != nullptr);
        REQUIRE(targetWindowGuard.get() != nullptr);
        ReceivingTarget target{pluginItem};
        RegistrationGuard registration{targetWindowGuard.get(), target};
        REQUIRE(registration.status() == S_OK);
        CursorGuard cursor;
        REQUIRE(cursor.captured());
        static_cast<void>(SetForegroundWindow(source.get()));
        const POINT sourcePoint = inside(source.get());
        REQUIRE(SetCursorPos(sourcePoint.x, sourcePoint.y) != FALSE);
        if (GetForegroundWindow() != source.get() || WindowFromPoint(sourcePoint) != source.get()) {
            std::fputs("SKIP: the plugin drag could not own its source window and cursor point\n", stderr);
            return;
        }

        pluginPanel = true;
        synchroRequests.store(0);
        pluginGlobal.Instance = reinterpret_cast<void *>(44);
        auto startupInfo = pluginStartup();
        startup(&startupInfo);
        PluginRuntimeGuard runtime{exit};

        MouseButtonGuard button;
        button.press();
        if (!pumpUntil([] { return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0; })) {
            button.release();
            std::fputs("SKIP: the desktop did not expose the injected left-button press\n", stderr);
            return;
        }
        auto press = farMouse({5, 5}, FROM_LEFT_1ST_BUTTON_PRESSED);
        CHECK(input(&press) == 0);
        auto threshold = farMouse({8, 5}, FROM_LEFT_1ST_BUTTON_PRESSED, MOUSE_MOVED);
        REQUIRE(input(&threshold) == 2);
        REQUIRE(synchroRequests.load() == 1);

        ProcessSynchroEventInfo event{};
        event.Event = SE_COMMONSYNCHRO;
        CHECK(synchro(&event) == 0);
        if (!pumpUntil([] { return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0; })) {
            button.release();
            std::fputs("SKIP: the desktop did not expose the tool window's injected left-button press\n", stderr);
            return;
        }

        const POINT targetPoint = inside(targetWindowGuard.get());
        REQUIRE(SetCursorPos(targetPoint.x, targetPoint.y) != FALSE);
        REQUIRE(pumpUntil([&target] { return target.entered(); }));
        CHECK(target.placeholdersReady());

        button.release();
        REQUIRE(pumpUntil([] { return synchroRequests.load() >= 2; }));
        CHECK(synchro(&event) == 0);
        REQUIRE(pumpUntil([&target] { return target.received(); }));
        CHECK(target.extracted());
        CHECK(synchroRequests.load() == 2);
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
        burlak::adapters::win::Screen screen;
        NoExtraction extraction;
        burlak::drag::DragSource source{policy,
                                        screen,
                                        extraction,
                                        burlak::core::Button::Left,
                                        reinterpret_cast<burlak::core::NativeWindow>(window.get()),
                                        false};
        const auto dragResult = burlak::adapters::shell::runDrag(window.get(), *data->data.Get(), source, true,
                                                                 burlak::adapters::shell::systemShellCalls());
        REQUIRE(dragResult.status == DRAGDROP_S_DROP);
        CHECK(target.received());
    }
}
