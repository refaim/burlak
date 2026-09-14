#include "adapters/shell/Shell.hpp"

#include <doctest/doctest.h>

#include <shellapi.h>

#include <filesystem>
#include <fstream>

namespace burlak::adapters::shell
{

    namespace
    {

        class DirectoryGuard
        {
          public:
            explicit DirectoryGuard(std::filesystem::path path) : path_{std::move(path)}
            {
                std::error_code ignored;
                std::filesystem::remove_all(path_, ignored);
            }

            ~DirectoryGuard()
            {
                std::error_code ignored;
                std::filesystem::remove_all(path_, ignored);
            }

            DirectoryGuard(const DirectoryGuard &) = delete;
            DirectoryGuard &operator=(const DirectoryGuard &) = delete;

            [[nodiscard]] const std::filesystem::path &path() const
            {
                return path_;
            }

          private:
            std::filesystem::path path_;
        };

        HRESULT WINAPI parseNull(PCWSTR, IBindCtx *, PIDLIST_ABSOLUTE *parsed, SFGAOF, SFGAOF *)
        {
            *parsed = nullptr;
            return S_OK;
        }

        HRESULT WINAPI parseExceptMissing(PCWSTR path, IBindCtx *context, PIDLIST_ABSOLUTE *parsed, SFGAOF attributes,
                                          SFGAOF *found)
        {
            if (std::wstring_view{path}.find(L"missing") != std::wstring_view::npos) {
                *parsed = nullptr;
                return E_FAIL;
            }
            return SHParseDisplayName(path, context, parsed, attributes, found);
        }

        HRESULT WINAPI failCreateArray(UINT, PCIDLIST_ABSOLUTE_ARRAY, IShellItemArray **)
        {
            return E_FAIL;
        }

        HRESULT failBind(IShellItemArray &, IDataObject **)
        {
            return E_FAIL;
        }

        UINT WINAPI failClipboardFormat(LPCWSTR)
        {
            return 0;
        }

        HGLOBAL WINAPI failGlobalAllocation(UINT, SIZE_T)
        {
            return nullptr;
        }

        LPVOID WINAPI failGlobalLock(HGLOBAL)
        {
            return nullptr;
        }

        HRESULT failSetData(IDataObject &, FORMATETC &, STGMEDIUM &, BOOL)
        {
            return E_FAIL;
        }

        HRESULT dragResult{DRAGDROP_S_DROP};
        DWORD draggedEffect{DROPEFFECT_COPY};
        DWORD allowedEffects{};

        HRESULT WINAPI fakeDrag(HWND, IDataObject *, IDropSource *, DWORD effects, DWORD *effect)
        {
            allowedEffects = effects;
            *effect = draggedEffect;
            return dragResult;
        }

        HRESULT failCreateOperation(IFileOperation **)
        {
            return E_FAIL;
        }

        HRESULT failQueue(IFileOperation &, IShellItem &, IShellItem &)
        {
            return E_FAIL;
        }

        HRESULT failOwner(IFileOperation &, HWND)
        {
            return E_FAIL;
        }

        HRESULT failFlags(IFileOperation &, DWORD)
        {
            return E_FAIL;
        }

        HWND recordedOwner{};
        DWORD recordedFlags{};

        HRESULT recordOwner(IFileOperation &, HWND owner)
        {
            recordedOwner = owner;
            return S_OK;
        }

        HRESULT recordFlags(IFileOperation &, DWORD flags)
        {
            recordedFlags = flags;
            return S_OK;
        }

        HRESULT passQueue(IFileOperation &, IShellItem &, IShellItem &)
        {
            return S_OK;
        }

        HRESULT failPerform(IFileOperation &)
        {
            return E_FAIL;
        }

        HRESULT passPerform(IFileOperation &)
        {
            return S_OK;
        }

        HRESULT failAborted(IFileOperation &, BOOL *)
        {
            return E_FAIL;
        }

        HRESULT reportAborted(IFileOperation &, BOOL *aborted)
        {
            *aborted = TRUE;
            return S_OK;
        }

        HRESULT reportComplete(IFileOperation &, BOOL *aborted)
        {
            *aborted = FALSE;
            return S_OK;
        }

        class TestDropSource final : public IDropSource
        {
          public:
            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **) override
            {
                return E_NOINTERFACE;
            }
            ULONG STDMETHODCALLTYPE AddRef() override
            {
                return 1;
            }
            ULONG STDMETHODCALLTYPE Release() override
            {
                return 1;
            }
            HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL, DWORD) override
            {
                return S_OK;
            }
            HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override
            {
                return DRAGDROP_S_USEDEFAULTCURSORS;
            }
        };

    } // namespace

    TEST_SUITE("shell adapter")
    {
        TEST_CASE("a real shell data object carries the requested file")
        {
            REQUIRE(SUCCEEDED(OleInitialize(nullptr)));
            const auto root = std::filesystem::canonical(std::filesystem::temp_directory_path()) / L"burlak-shell-data";
            std::filesystem::create_directories(root);
            const auto file = root / L"one.txt";
            {
                std::ofstream stream{file};
            }
            const std::vector<std::wstring> paths{file.wstring()};

            auto data = makeDataObject(paths);
            REQUIRE(data.has_value());
            CHECK(data->parsedPaths == paths.size());
            FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            STGMEDIUM medium{};
            REQUIRE(SUCCEEDED(data->data->GetData(&format, &medium)));
            const auto drop = static_cast<HDROP>(GlobalLock(medium.hGlobal));
            REQUIRE(drop != nullptr);
            wchar_t path[MAX_PATH]{};
            CHECK(DragQueryFileW(drop, 0, path, MAX_PATH) > 0);
            CHECK(std::filesystem::canonical(std::filesystem::path{path}) == std::filesystem::canonical(file));

            const auto preferredFormat = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
            REQUIRE(preferredFormat != 0);
            FORMATETC preferred{static_cast<CLIPFORMAT>(preferredFormat), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            const auto realQuery = data->data->QueryGetData(&preferred);
            STGMEDIUM realPreferredMedium{};
            const auto realGet = data->data->GetData(&preferred, &realPreferredMedium);
            if (SUCCEEDED(realGet)) {
                const auto value = static_cast<const DWORD *>(GlobalLock(realPreferredMedium.hGlobal));
                if (value != nullptr) {
                    GlobalUnlock(realPreferredMedium.hGlobal);
                }
                ReleaseStgMedium(&realPreferredMedium);
            }
            CHECK(realQuery != S_OK);
            CHECK(FAILED(realGet));

            auto placeholders = makeDataObject(paths, core::Effect::Copy);
            REQUIRE(placeholders.has_value());
            STGMEDIUM preferredMedium{};
            REQUIRE(SUCCEEDED(placeholders->data->GetData(&preferred, &preferredMedium)));
            const auto preferredValue = static_cast<const DWORD *>(GlobalLock(preferredMedium.hGlobal));
            REQUIRE(preferredValue != nullptr);
            CHECK(*preferredValue == DROPEFFECT_COPY);
            GlobalUnlock(preferredMedium.hGlobal);
            ReleaseStgMedium(&preferredMedium);

            TestDropSource source;
            auto calls = systemShellCalls();
            calls.doDragDrop = fakeDrag;
            dragResult = DRAGDROP_S_DROP;
            draggedEffect = DROPEFFECT_MOVE;
            CHECK(runDrag(nullptr, *data->data.Get(), source, true, calls) ==
                  core::DragLoopOutcome{DRAGDROP_S_DROP, DROPEFFECT_MOVE});
            CHECK(allowedEffects == (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK));
            dragResult = DRAGDROP_S_CANCEL;
            CHECK(runDrag(nullptr, *data->data.Get(), source, false, calls) ==
                  core::DragLoopOutcome{DRAGDROP_S_CANCEL, DROPEFFECT_MOVE});
            CHECK(allowedEffects == (DROPEFFECT_COPY | DROPEFFECT_MOVE));
            dragResult = E_FAIL;
            CHECK(runDrag(nullptr, *data->data.Get(), source, true, calls) ==
                  core::DragLoopOutcome{E_FAIL, DROPEFFECT_MOVE});

            Shell shell{calls};
            auto opaque = shell.makeDataObject(paths);
            REQUIRE(opaque.has_value());
            CHECK(opaque->parsedPaths == paths.size());
            dragResult = DRAGDROP_S_DROP;
            CHECK(shell.runDrag(0, *opaque->data, reinterpret_cast<std::uintptr_t>(&source), true) ==
                  core::DragLoopOutcome{DRAGDROP_S_DROP, DROPEFFECT_MOVE});
            dragResult = E_FAIL;
            CHECK(shell.runDrag(0, *opaque->data, reinterpret_cast<std::uintptr_t>(&source), false) ==
                  core::DragLoopOutcome{E_FAIL, DROPEFFECT_MOVE});
            GlobalUnlock(medium.hGlobal);
            ReleaseStgMedium(&medium);

            data = std::unexpected(core::Error::Unavailable);
            std::filesystem::remove_all(root);
            OleUninitialize();
        }

        TEST_CASE("no parseable paths is an expected data-object failure")
        {
            REQUIRE(SUCCEEDED(OleInitialize(nullptr)));
            const std::vector<std::wstring> paths{L"Z:\\a-path-that-does-not-exist\\missing.txt"};
            CHECK(makeDataObject(paths).error() == core::Error::NoSelection);

            auto calls = systemShellCalls();
            calls.parseDisplayName = parseNull;
            CHECK(makeDataObject(paths, calls) == std::unexpected(core::Error::NoSelection));
            CHECK(Shell{calls}.makeDataObject(paths) == std::unexpected(core::Error::NoSelection));
            OleUninitialize();
        }

        TEST_CASE("shell data-object construction translates array and binding failures")
        {
            REQUIRE(SUCCEEDED(OleInitialize(nullptr)));
            const auto root = std::filesystem::temp_directory_path() / L"burlak-shell-failures";
            std::filesystem::create_directories(root);
            const auto file = root / L"one.txt";
            {
                std::ofstream stream{file};
            }
            const std::vector<std::wstring> paths{file.wstring()};

            auto calls = systemShellCalls();
            calls.createItemArray = failCreateArray;
            CHECK(makeDataObject(paths, calls) == std::unexpected(core::Error::Unavailable));
            calls = systemShellCalls();
            calls.bindDataObject = failBind;
            CHECK(makeDataObject(paths, calls) == std::unexpected(core::Error::Unavailable));
            calls = systemShellCalls();
            calls.registerClipboardFormat = failClipboardFormat;
            CHECK(makeDataObject(paths, core::Effect::Copy, calls) == std::unexpected(core::Error::Unavailable));
            calls = systemShellCalls();
            calls.globalAlloc = failGlobalAllocation;
            CHECK(makeDataObject(paths, core::Effect::Copy, calls) == std::unexpected(core::Error::Unavailable));
            calls = systemShellCalls();
            calls.globalLock = failGlobalLock;
            CHECK(makeDataObject(paths, core::Effect::Copy, calls) == std::unexpected(core::Error::Unavailable));
            calls = systemShellCalls();
            calls.setData = failSetData;
            CHECK(makeDataObject(paths, core::Effect::Copy, calls) == std::unexpected(core::Error::Unavailable));

            std::filesystem::remove_all(root);
            OleUninitialize();
        }

        TEST_CASE("data-object construction reports exactly how many paths were advertised")
        {
            REQUIRE(SUCCEEDED(OleInitialize(nullptr)));
            const auto root = std::filesystem::temp_directory_path() / L"burlak-shell-partial";
            std::filesystem::create_directories(root);
            const auto file = root / L"one.txt";
            {
                std::ofstream stream{file};
            }
            const std::vector<std::wstring> paths{file.wstring(), L"missing"};
            auto calls = systemShellCalls();
            calls.parseDisplayName = parseExceptMissing;

            const auto data = makeDataObject(paths, calls);
            REQUIRE(data.has_value());
            CHECK(data->parsedPaths == 1);

            std::filesystem::remove_all(root);
            OleUninitialize();
        }

        TEST_CASE("IFileOperation copies a real file between temporary directories")
        {
            REQUIRE(SUCCEEDED(OleInitialize(nullptr)));
            DirectoryGuard root{std::filesystem::temp_directory_path() /
                                (L"burlak-shell-copy-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                 std::to_wstring(GetTickCount64()))};
            const auto source = root.path() / L"source";
            const auto destination = root.path() / L"destination";
            std::filesystem::create_directories(source);
            std::filesystem::create_directories(destination);
            const auto file = source / L"copied.txt";
            {
                std::ofstream stream{file};
            }
            const std::vector<std::wstring> paths{file.wstring()};

            Shell shell;
            CHECK(shell.copy(paths, destination.wstring(), core::Effect::None, 0) ==
                  std::unexpected(core::Error::ForeignCallFailed));
            CHECK(shell.copy(paths, destination.wstring(), core::Effect::Link, 0) ==
                  std::unexpected(core::Error::ForeignCallFailed));
            CHECK(shell.copy(paths, destination.wstring(), core::Effect::Copy, 0).has_value());
            CHECK(std::filesystem::exists(destination / file.filename()));

            const auto moved = source / L"moved.txt";
            {
                std::ofstream stream{moved};
            }
            const std::vector<std::wstring> movedPaths{moved.wstring()};
            CHECK(shell.copy(movedPaths, destination.wstring(), core::Effect::Move, 0).has_value());
            CHECK(std::filesystem::exists(destination / moved.filename()));
            CHECK_FALSE(std::filesystem::exists(moved));

            CHECK(shell.copy(paths, L"Z:\\missing-destination", core::Effect::Copy, 0) ==
                  std::unexpected(core::Error::DirectoryUnavailable));
            const std::vector<std::wstring> missing{L"Z:\\missing-source\\file.txt"};
            CHECK(shell.copy(missing, destination.wstring(), core::Effect::Copy, 0) ==
                  std::unexpected(core::Error::ForeignCallFailed));

            OleUninitialize();
        }

        TEST_CASE("file-operation failures are translated at every foreign call")
        {
            REQUIRE(SUCCEEDED(OleInitialize(nullptr)));
            const auto root = std::filesystem::temp_directory_path() / L"burlak-shell-operation-failures";
            const auto source = root / L"source";
            const auto destination = root / L"destination";
            std::filesystem::create_directories(source);
            std::filesystem::create_directories(destination);
            const auto file = source / L"one.txt";
            {
                std::ofstream stream{file};
            }
            const std::vector<std::wstring> paths{file.wstring()};

            auto calls = systemShellCalls();
            calls.createOperation = failCreateOperation;
            CHECK(Shell{calls}.copy(paths, destination.wstring(), core::Effect::Copy, 0) ==
                  std::unexpected(core::Error::Unavailable));

            calls = systemShellCalls();
            calls.setOwner = failOwner;
            CHECK(Shell{calls}.copy(paths, destination.wstring(), core::Effect::Copy, 0) ==
                  std::unexpected(core::Error::Unavailable));
            calls = systemShellCalls();
            calls.setFlags = failFlags;
            CHECK(Shell{calls}.copy(paths, destination.wstring(), core::Effect::Copy, 0) ==
                  std::unexpected(core::Error::Unavailable));

            calls = systemShellCalls();
            calls.setOwner = recordOwner;
            calls.setFlags = recordFlags;
            calls.copyItem = passQueue;
            calls.perform = passPerform;
            calls.getAborted = reportComplete;
            recordedOwner = nullptr;
            recordedFlags = 0;
            CHECK(Shell{calls}.copy(paths, destination.wstring(), core::Effect::Copy, 123).has_value());
            CHECK(recordedOwner == reinterpret_cast<HWND>(123));
            CHECK((recordedFlags & FOF_ALLOWUNDO) != 0);
            CHECK((recordedFlags & FOFX_SHOWELEVATIONPROMPT) != 0);

            calls = systemShellCalls();
            calls.copyItem = failQueue;
            CHECK(Shell{calls}.copy(paths, destination.wstring(), core::Effect::Copy, 0) ==
                  std::unexpected(core::Error::ForeignCallFailed));
            calls = systemShellCalls();
            calls.moveItem = failQueue;
            CHECK(Shell{calls}.copy(paths, destination.wstring(), core::Effect::Move, 0) ==
                  std::unexpected(core::Error::ForeignCallFailed));

            calls = systemShellCalls();
            calls.copyItem = passQueue;
            calls.perform = failPerform;
            CHECK(Shell{calls}.copy(paths, destination.wstring(), core::Effect::Copy, 0) ==
                  std::unexpected(core::Error::ForeignCallFailed));

            calls = systemShellCalls();
            calls.copyItem = passQueue;
            calls.perform = passPerform;
            calls.getAborted = failAborted;
            CHECK(Shell{calls}.copy(paths, destination.wstring(), core::Effect::Copy, 0) ==
                  std::unexpected(core::Error::ForeignCallFailed));
            calls.getAborted = reportAborted;
            CHECK(Shell{calls}.copy(paths, destination.wstring(), core::Effect::Copy, 0) ==
                  std::unexpected(core::Error::ForeignCallFailed));

            std::filesystem::remove_all(root);
            OleUninitialize();
        }
    }

} // namespace burlak::adapters::shell
