#include "adapters/shell/Shell.hpp"

#include "core/Policies.hpp"

#include <shlobj.h>

#include <memory>
#include <vector>

namespace burlak::adapters::shell
{

    namespace
    {

        struct PidlFreer
        {
            void operator()(ITEMIDLIST *pidl) const noexcept
            {
                CoTaskMemFree(pidl);
            }
        };

        using UniquePidl = std::unique_ptr<ITEMIDLIST, PidlFreer>;

        class ShellDragData final : public core::IShell::DragData
        {
          public:
            explicit ShellDragData(DataObject data) : data_{std::move(data)}
            {
            }

            [[nodiscard]] std::uintptr_t nativeHandle() const override
            {
                return reinterpret_cast<std::uintptr_t>(data_.Get());
            }

            DataObject data_;
        };

        HRESULT bindDataObject(IShellItemArray &array, IDataObject **data)
        {
            return array.BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(data));
        }

        HRESULT createOperation(IFileOperation **operation)
        {
            return CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(operation));
        }

        HRESULT createItem(PCWSTR path, IShellItem **item)
        {
            return SHCreateItemFromParsingName(path, nullptr, IID_PPV_ARGS(item));
        }

        HRESULT copyItem(IFileOperation &operation, IShellItem &source, IShellItem &destination)
        {
            return operation.CopyItem(&source, &destination, nullptr, nullptr);
        }

        HRESULT moveItem(IFileOperation &operation, IShellItem &source, IShellItem &destination)
        {
            return operation.MoveItem(&source, &destination, nullptr, nullptr);
        }

        HRESULT perform(IFileOperation &operation)
        {
            return operation.PerformOperations();
        }

        HRESULT getAborted(IFileOperation &operation, BOOL *aborted)
        {
            return operation.GetAnyOperationsAborted(aborted);
        }

        const ShellCalls calls{SHParseDisplayName,
                               SHCreateShellItemArrayFromIDLists,
                               bindDataObject,
                               SHDoDragDrop,
                               createOperation,
                               createItem,
                               copyItem,
                               moveItem,
                               perform,
                               getAborted};

    } // namespace

    std::expected<PreparedDataObject, core::Error> makeDataObject(std::span<const std::wstring> paths)
    {
        return makeDataObject(paths, calls);
    }

    std::expected<PreparedDataObject, core::Error> makeDataObject(std::span<const std::wstring> paths,
                                                                  const ShellCalls &api)
    {
        std::vector<UniquePidl> ownedPidls;
        std::vector<PCIDLIST_ABSOLUTE> pidls;
        ownedPidls.reserve(paths.size());
        pidls.reserve(paths.size());
        for (const auto &path : paths) {
            PIDLIST_ABSOLUTE parsed{};
            if (SUCCEEDED(api.parseDisplayName(path.c_str(), nullptr, &parsed, 0, nullptr)) && parsed != nullptr) {
                ownedPidls.emplace_back(parsed);
                pidls.push_back(parsed);
            }
        }
        if (pidls.empty()) {
            return std::unexpected(core::Error::NoSelection);
        }

        Microsoft::WRL::ComPtr<IShellItemArray> array;
        if (FAILED(api.createItemArray(static_cast<UINT>(pidls.size()), pidls.data(), array.GetAddressOf()))) {
            return std::unexpected(core::Error::Unavailable);
        }
        DataObject data;
        if (FAILED(api.bindDataObject(*array.Get(), data.GetAddressOf()))) {
            return std::unexpected(core::Error::Unavailable);
        }
        return PreparedDataObject{.data = std::move(data), .parsedPaths = pidls.size()};
    }

    core::DragLoopOutcome runDrag(HWND owner, IDataObject &data, IDropSource &source, const ShellCalls &api)
    {
        DWORD effect{};
        const HRESULT result =
            api.doDragDrop(owner, &data, &source, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
        return {.status = result, .effect = effect};
    }

    const ShellCalls &systemShellCalls()
    {
        return calls;
    }

    Shell::Shell() : calls_{calls}
    {
    }

    Shell::Shell(const ShellCalls &api) : calls_{api}
    {
    }

    std::expected<core::IShell::PreparedDrag, core::Error> Shell::makeDataObject(std::span<const std::wstring> paths)
    {
        return shell::makeDataObject(paths, calls_).transform([](PreparedDataObject prepared) {
            return PreparedDrag{.data = std::make_unique<ShellDragData>(std::move(prepared.data)),
                                .parsedPaths = prepared.parsedPaths};
        });
    }

    core::DragLoopOutcome Shell::runDrag(core::NativeWindow owner, DragData &data, std::uintptr_t source)
    {
        auto &nativeData = *reinterpret_cast<IDataObject *>(data.nativeHandle());
        auto &dropSource = *reinterpret_cast<IDropSource *>(source);
        return shell::runDrag(reinterpret_cast<HWND>(owner), nativeData, dropSource, calls_);
    }

    std::expected<void, core::Error> Shell::copy(std::span<const std::wstring> paths, std::wstring_view destination,
                                                 core::Effect effect)
    {
        if (effect == core::Effect::None) {
            return std::unexpected(core::Error::ForeignCallFailed);
        }
        Microsoft::WRL::ComPtr<IFileOperation> operation;
        if (FAILED(calls_.createOperation(operation.GetAddressOf()))) {
            return std::unexpected(core::Error::Unavailable);
        }
        static_cast<void>(operation->SetOperationFlags(FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI));

        Microsoft::WRL::ComPtr<IShellItem> destinationItem;
        const std::wstring destinationText{destination};
        if (FAILED(calls_.createItem(destinationText.c_str(), destinationItem.GetAddressOf()))) {
            return std::unexpected(core::Error::DirectoryUnavailable);
        }
        for (const auto &path : paths) {
            Microsoft::WRL::ComPtr<IShellItem> sourceItem;
            if (FAILED(calls_.createItem(path.c_str(), sourceItem.GetAddressOf()))) {
                return std::unexpected(core::Error::ForeignCallFailed);
            }
            const HRESULT queued = effect == core::Effect::Copy
                                       ? calls_.copyItem(*operation.Get(), *sourceItem.Get(), *destinationItem.Get())
                                       : calls_.moveItem(*operation.Get(), *sourceItem.Get(), *destinationItem.Get());
            if (FAILED(queued)) {
                return std::unexpected(core::Error::ForeignCallFailed);
            }
        }
        if (FAILED(calls_.perform(*operation.Get()))) {
            return std::unexpected(core::Error::ForeignCallFailed);
        }
        BOOL aborted{};
        const HRESULT completion = calls_.getAborted(*operation.Get(), &aborted);
        return core::expectedOutcome(SUCCEEDED(completion) && !aborted, core::Error::ForeignCallFailed);
    }

} // namespace burlak::adapters::shell
