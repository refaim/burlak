#include "adapters/shell/Shell.hpp"

#include <doctest/doctest.h>

#include <shellapi.h>

#include <filesystem>
#include <fstream>

namespace burlak::adapters::shell
{

    namespace
    {

        HRESULT WINAPI parseNull(PCWSTR, IBindCtx *, PIDLIST_ABSOLUTE *parsed, SFGAOF, SFGAOF *)
        {
            *parsed = nullptr;
            return S_OK;
        }

        HRESULT WINAPI failCreateArray(UINT, PCIDLIST_ABSOLUTE_ARRAY, IShellItemArray **)
        {
            return E_FAIL;
        }

        HRESULT failBind(IShellItemArray &, IDataObject **)
        {
            return E_FAIL;
        }

        HRESULT dragResult{DRAGDROP_S_DROP};
        DWORD draggedEffect{DROPEFFECT_COPY};

        HRESULT WINAPI fakeDrag(HWND, IDataObject *, IDropSource *, DWORD, DWORD *effect)
        {
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
            FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            STGMEDIUM medium{};
            REQUIRE(SUCCEEDED((*data)->GetData(&format, &medium)));
            const auto drop = static_cast<HDROP>(GlobalLock(medium.hGlobal));
            REQUIRE(drop != nullptr);
            wchar_t path[MAX_PATH]{};
            CHECK(DragQueryFileW(drop, 0, path, MAX_PATH) > 0);
            CHECK(std::filesystem::canonical(std::filesystem::path{path}) == std::filesystem::canonical(file));

            TestDropSource source;
            auto calls = systemShellCalls();
            calls.doDragDrop = fakeDrag;
            dragResult = DRAGDROP_S_DROP;
            draggedEffect = DROPEFFECT_MOVE;
            CHECK(runDrag(nullptr, *data.value().Get(), source, calls) ==
                  core::DragLoopOutcome{DRAGDROP_S_DROP, DROPEFFECT_MOVE});
            dragResult = DRAGDROP_S_CANCEL;
            CHECK(runDrag(nullptr, *data.value().Get(), source, calls) ==
                  core::DragLoopOutcome{DRAGDROP_S_CANCEL, DROPEFFECT_MOVE});
            dragResult = E_FAIL;
            CHECK(runDrag(nullptr, *data.value().Get(), source, calls) ==
                  core::DragLoopOutcome{E_FAIL, DROPEFFECT_MOVE});

            Shell shell{calls};
            auto opaque = shell.makeDataObject(paths);
            REQUIRE(opaque.has_value());
            dragResult = DRAGDROP_S_DROP;
            CHECK(shell.runDrag(0, **opaque, reinterpret_cast<std::uintptr_t>(&source)) ==
                  core::DragLoopOutcome{DRAGDROP_S_DROP, DROPEFFECT_MOVE});
            dragResult = E_FAIL;
            CHECK(shell.runDrag(0, **opaque, reinterpret_cast<std::uintptr_t>(&source)) ==
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

            std::filesystem::remove_all(root);
            OleUninitialize();
        }

        TEST_CASE("IFileOperation copies a real file between temporary directories")
        {
            REQUIRE(SUCCEEDED(OleInitialize(nullptr)));
            const auto root = std::filesystem::temp_directory_path() / L"burlak-shell-copy";
            const auto source = root / L"source";
            const auto destination = root / L"destination";
            std::filesystem::create_directories(source);
            std::filesystem::create_directories(destination);
            const auto file = source / L"copied.txt";
            {
                std::ofstream stream{file};
            }
            const std::vector<std::wstring> paths{file.wstring()};

            Shell shell;
            CHECK(shell.copy(paths, destination.wstring(), core::Effect::None) ==
                  std::unexpected(core::Error::ForeignCallFailed));
            CHECK(shell.copy(paths, destination.wstring(), core::Effect::Copy).has_value());
            CHECK(std::filesystem::exists(destination / file.filename()));

            const auto moved = source / L"moved.txt";
            {
                std::ofstream stream{moved};
            }
            const std::vector<std::wstring> movedPaths{moved.wstring()};
            CHECK(shell.copy(movedPaths, destination.wstring(), core::Effect::Move).has_value());
            CHECK(std::filesystem::exists(destination / moved.filename()));
            CHECK_FALSE(std::filesystem::exists(moved));

            CHECK(shell.copy(paths, L"Z:\\missing-destination", core::Effect::Copy) ==
                  std::unexpected(core::Error::DirectoryUnavailable));
            const std::vector<std::wstring> missing{L"Z:\\missing-source\\file.txt"};
            CHECK(shell.copy(missing, destination.wstring(), core::Effect::Copy) ==
                  std::unexpected(core::Error::ForeignCallFailed));

            std::filesystem::remove_all(root);
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
            CHECK(Shell{calls}.copy(paths, destination.wstring(), core::Effect::Copy) ==
                  std::unexpected(core::Error::Unavailable));

            calls = systemShellCalls();
            calls.copyItem = failQueue;
            CHECK(Shell{calls}.copy(paths, destination.wstring(), core::Effect::Copy) ==
                  std::unexpected(core::Error::ForeignCallFailed));
            calls = systemShellCalls();
            calls.moveItem = failQueue;
            CHECK(Shell{calls}.copy(paths, destination.wstring(), core::Effect::Move) ==
                  std::unexpected(core::Error::ForeignCallFailed));

            calls = systemShellCalls();
            calls.copyItem = passQueue;
            calls.perform = failPerform;
            CHECK(Shell{calls}.copy(paths, destination.wstring(), core::Effect::Copy) ==
                  std::unexpected(core::Error::ForeignCallFailed));

            calls = systemShellCalls();
            calls.copyItem = passQueue;
            calls.perform = passPerform;
            calls.getAborted = failAborted;
            CHECK(Shell{calls}.copy(paths, destination.wstring(), core::Effect::Copy) ==
                  std::unexpected(core::Error::ForeignCallFailed));
            calls.getAborted = reportAborted;
            CHECK(Shell{calls}.copy(paths, destination.wstring(), core::Effect::Copy) ==
                  std::unexpected(core::Error::ForeignCallFailed));

            std::filesystem::remove_all(root);
            OleUninitialize();
        }
    }

} // namespace burlak::adapters::shell
