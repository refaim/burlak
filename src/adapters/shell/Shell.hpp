#pragma once

#include "core/Interfaces.hpp"

#include <shlobj.h>
#include <shobjidl.h>
#include <windows.h>
#include <wrl/client.h>

#include <expected>
#include <span>
#include <string>

namespace burlak::adapters::shell
{

    using DataObject = Microsoft::WRL::ComPtr<IDataObject>;

    struct PreparedDataObject
    {
        DataObject data;
        std::size_t parsedPaths{};
    };

    struct ShellCalls
    {
        decltype(&SHParseDisplayName) parseDisplayName;
        decltype(&SHCreateShellItemArrayFromIDLists) createItemArray;
        HRESULT (*bindDataObject)(IShellItemArray &array, IDataObject **data);
        decltype(&SHDoDragDrop) doDragDrop;
        HRESULT (*createOperation)(IFileOperation **operation);
        HRESULT (*setOwner)(IFileOperation &operation, HWND owner);
        HRESULT (*setFlags)(IFileOperation &operation, DWORD flags);
        HRESULT (*createItem)(PCWSTR path, IShellItem **item);
        HRESULT (*copyItem)(IFileOperation &operation, IShellItem &source, IShellItem &destination);
        HRESULT (*moveItem)(IFileOperation &operation, IShellItem &source, IShellItem &destination);
        HRESULT (*perform)(IFileOperation &operation);
        HRESULT (*getAborted)(IFileOperation &operation, BOOL *aborted);
    };

    [[nodiscard]] std::expected<PreparedDataObject, core::Error> makeDataObject(std::span<const std::wstring> paths);
    [[nodiscard]] std::expected<PreparedDataObject, core::Error> makeDataObject(std::span<const std::wstring> paths,
                                                                                const ShellCalls &calls);
    [[nodiscard]] core::DragLoopOutcome runDrag(HWND owner, IDataObject &data, IDropSource &source, bool allowLink,
                                                const ShellCalls &calls);
    [[nodiscard]] const ShellCalls &systemShellCalls();

    class Shell final : public core::IShell
    {
      public:
        Shell();
        explicit Shell(const ShellCalls &calls);

        [[nodiscard]] std::expected<PreparedDrag, core::Error> makeDataObject(
            std::span<const std::wstring> paths) override;
        [[nodiscard]] core::DragLoopOutcome runDrag(core::NativeWindow owner, DragData &data, std::uintptr_t source,
                                                    bool allowLink) override;
        [[nodiscard]] std::expected<void, core::Error> copy(std::span<const std::wstring> paths,
                                                            std::wstring_view destination, core::Effect effect,
                                                            core::NativeWindow owner) override;

      private:
        const ShellCalls &calls_;
    };

} // namespace burlak::adapters::shell
