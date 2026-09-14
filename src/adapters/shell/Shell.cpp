#include "adapters/shell/Shell.hpp"

#include "core/Policies.hpp"

#include <shlobj.h>

#include <array>
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

        struct GlobalFreer
        {
            decltype(&GlobalFree) release;

            void operator()(void *memory) const noexcept
            {
                static_cast<void>(release(memory));
            }
        };

        using UniqueGlobal = std::unique_ptr<void, GlobalFreer>;

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

        HRESULT setData(IDataObject &data, FORMATETC &format, STGMEDIUM &medium, BOOL release)
        {
            return data.SetData(&format, &medium, release);
        }

        [[nodiscard]] std::expected<void, core::Error> setPreferredEffect(IDataObject &data, core::Effect effect,
                                                                          const ShellCalls &api)
        {
            const auto clipboardFormat = api.registerClipboardFormat(CFSTR_PREFERREDDROPEFFECT);
            if (clipboardFormat == 0) {
                return std::unexpected(core::Error::Unavailable);
            }
            UniqueGlobal memory{api.globalAlloc(GMEM_MOVEABLE, sizeof(DWORD)), GlobalFreer{api.globalFree}};
            if (!memory) {
                return std::unexpected(core::Error::Unavailable);
            }
            const auto value = static_cast<DWORD *>(api.globalLock(memory.get()));
            if (value == nullptr) {
                return std::unexpected(core::Error::Unavailable);
            }
            constexpr std::array<DWORD, 4> nativeEffects{DROPEFFECT_NONE, DROPEFFECT_COPY, DROPEFFECT_MOVE,
                                                         DROPEFFECT_LINK};
            *value = nativeEffects.at(static_cast<std::size_t>(effect));
            static_cast<void>(api.globalUnlock(memory.get()));

            FORMATETC format{static_cast<CLIPFORMAT>(clipboardFormat), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            STGMEDIUM medium{};
            medium.tymed = TYMED_HGLOBAL;
            medium.hGlobal = memory.get();
            if (FAILED(api.setData(data, format, medium, TRUE))) {
                return std::unexpected(core::Error::Unavailable);
            }
            static_cast<void>(memory.release());
            return {};
        }

        HRESULT createOperation(IFileOperation **operation)
        {
            return CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(operation));
        }

        HRESULT createItem(PCWSTR path, IShellItem **item)
        {
            return SHCreateItemFromParsingName(path, nullptr, IID_PPV_ARGS(item));
        }

        HRESULT setOwner(IFileOperation &operation, HWND owner)
        {
            return operation.SetOwnerWindow(owner);
        }

        HRESULT setFlags(IFileOperation &operation, DWORD flags)
        {
            return operation.SetOperationFlags(flags);
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
                               RegisterClipboardFormatW,
                               GlobalAlloc,
                               GlobalLock,
                               GlobalUnlock,
                               GlobalFree,
                               setData,
                               SHDoDragDrop,
                               createOperation,
                               setOwner,
                               setFlags,
                               createItem,
                               copyItem,
                               moveItem,
                               perform,
                               getAborted};

    } // namespace

    std::expected<PreparedDataObject, core::Error> makeDataObject(std::span<const std::wstring> paths)
    {
        return makeDataObject(paths, std::nullopt, calls);
    }

    std::expected<PreparedDataObject, core::Error> makeDataObject(std::span<const std::wstring> paths,
                                                                  std::optional<core::Effect> preferredEffect)
    {
        return makeDataObject(paths, preferredEffect, calls);
    }

    std::expected<PreparedDataObject, core::Error> makeDataObject(std::span<const std::wstring> paths,
                                                                  const ShellCalls &api)
    {
        return makeDataObject(paths, std::nullopt, api);
    }

    std::expected<PreparedDataObject, core::Error> makeDataObject(std::span<const std::wstring> paths,
                                                                  std::optional<core::Effect> preferredEffect,
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
        if (preferredEffect && !setPreferredEffect(*data.Get(), *preferredEffect, api)) {
            return std::unexpected(core::Error::Unavailable);
        }
        return PreparedDataObject{.data = std::move(data), .parsedPaths = pidls.size()};
    }

    core::DragLoopOutcome runDrag(HWND owner, IDataObject &data, IDropSource &source, bool allowLink,
                                  const ShellCalls &api)
    {
        DWORD effect{};
        const DWORD allowed = DROPEFFECT_COPY | DROPEFFECT_MOVE | (allowLink ? DROPEFFECT_LINK : 0);
        const HRESULT result = api.doDragDrop(owner, &data, &source, allowed, &effect);
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

    std::expected<core::IShell::PreparedDrag, core::Error> Shell::makeDataObject(
        std::span<const std::wstring> paths, std::optional<core::Effect> preferredEffect)
    {
        return shell::makeDataObject(paths, preferredEffect, calls_).transform([](PreparedDataObject prepared) {
            return PreparedDrag{.data = std::make_unique<ShellDragData>(std::move(prepared.data)),
                                .parsedPaths = prepared.parsedPaths};
        });
    }

    core::DragLoopOutcome Shell::runDrag(core::NativeWindow owner, DragData &data, std::uintptr_t source,
                                         bool allowLink)
    {
        auto &nativeData = *reinterpret_cast<IDataObject *>(data.nativeHandle());
        auto &dropSource = *reinterpret_cast<IDropSource *>(source);
        return shell::runDrag(reinterpret_cast<HWND>(owner), nativeData, dropSource, allowLink, calls_);
    }

    std::expected<void, core::Error> Shell::copy(std::span<const std::wstring> paths, std::wstring_view destination,
                                                 core::Effect effect, core::NativeWindow owner)
    {
        if (effect != core::Effect::Copy && effect != core::Effect::Move) {
            return std::unexpected(core::Error::ForeignCallFailed);
        }
        Microsoft::WRL::ComPtr<IFileOperation> operation;
        if (FAILED(calls_.createOperation(operation.GetAddressOf()))) {
            return std::unexpected(core::Error::Unavailable);
        }
        constexpr DWORD flags = FOF_ALLOWUNDO | FOFX_SHOWELEVATIONPROMPT;
        if (FAILED(calls_.setOwner(*operation.Get(), reinterpret_cast<HWND>(owner))) ||
            FAILED(calls_.setFlags(*operation.Get(), flags))) {
            return std::unexpected(core::Error::Unavailable);
        }

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
