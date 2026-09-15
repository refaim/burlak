#include "adapters/far/PluginCall.hpp"

#include "core/Policies.hpp"

#include <windows.h>

#include <plugin.hpp>

#include <bit>
#include <memory>
#include <string>
#include <vector>

namespace burlak::adapters::far_api
{

    namespace
    {

        using GetFilesFunction = intptr_t(WINAPI *)(GetFilesInfo *);

        struct ModuleCloser
        {
            void operator()(HINSTANCE__ *module) const noexcept
            {
                FreeLibrary(module);
            }
        };

        using UniqueModule = std::unique_ptr<HINSTANCE__, ModuleCloser>;

        // A third-party panel plugin is outside Burlak's trust boundary. This function deliberately has
        // no C++ objects so MSVC's SEH restriction cannot bypass destructors during an access violation.
        int markCrash(bool &crashed) noexcept
        {
            crashed = true;
            return EXCEPTION_EXECUTE_HANDLER;
        }

        intptr_t callGuarded(GetFilesFunction function, GetFilesInfo *info, wchar_t *destinationCopy,
                             std::size_t destinationCapacity, std::size_t &destinationLength, bool &crashed) noexcept
        {
            intptr_t result{};
            __try {
                result = function(info);
                if (result != 0 && info->DestPath != nullptr) {
                    while (destinationLength < destinationCapacity) {
                        wchar_t character{};
                        const auto address = info->DestPath + destinationLength;
                        if (ReadProcessMemory(GetCurrentProcess(), address, &character, sizeof(character), nullptr) ==
                            FALSE) {
                            crashed = true;
                            break;
                        }
                        if (character == L'\0') {
                            break;
                        }
                        destinationCopy[destinationLength] = character;
                        ++destinationLength;
                    }
                    if (destinationLength == destinationCapacity) {
                        result = 0;
                    }
                }
            } __except (markCrash(crashed)) {
            }
            return result;
        }

    } // namespace

    bool hasGetFilesExport(const core::PluginModule &module)
    {
        UniqueModule loaded{LoadLibraryW(module.path.c_str())};
        return loaded && GetProcAddress(loaded.get(), "GetFilesW") != nullptr;
    }

    std::expected<std::wstring, core::Error> callPluginGetFiles(core::PanelHandle panel,
                                                                std::span<const core::Item> items,
                                                                const core::PluginModule &module,
                                                                std::wstring_view destination)
    {
        UniqueModule loaded{LoadLibraryW(module.path.c_str())};
        if (!loaded) {
            return std::unexpected(core::Error::ForeignCallFailed);
        }

        const auto function = std::bit_cast<GetFilesFunction>(GetProcAddress(loaded.get(), "GetFilesW"));
        if (function == nullptr) {
            return std::unexpected(core::Error::ForeignCallFailed);
        }

        // Far forwards every CreatePluginItemList record unchanged through PluginManager::GetFiles, including
        // pointers into each FCTL_GETSELECTEDPANELITEM buffer; the plan owns those buffers for this call
        // (Far sources: far/filelist.cpp, FileList::CreatePluginItemList; far/plugins.cpp,
        // PluginManager::GetFiles).
        std::vector<PluginPanelItem> foreignItems(items.size());
        for (std::size_t index = 0; index < items.size(); ++index) {
            if (items[index].native.size() < sizeof(PluginPanelItem)) {
                return std::unexpected(core::Error::ForeignCallFailed);
            }
            foreignItems[index] = *reinterpret_cast<const PluginPanelItem *>(items[index].native.data());
        }

        GetFilesInfo info{};
        info.StructSize = sizeof(info);
        info.hPanel = reinterpret_cast<HANDLE>(panel);
        info.PanelItem = foreignItems.data();
        info.ItemsNumber = foreignItems.size();
        info.Move = FALSE;
        const std::wstring destinationText{destination};
        info.DestPath = destinationText.c_str();
        info.OpMode = OPM_SILENT;
        info.Instance = reinterpret_cast<void *>(module.instance);

        bool crashed = false;
        // Windows paths cannot exceed 32,767 UTF-16 code units. This caller-owned buffer lets the SEH frame copy
        // a rewritten plugin pointer before either C++ code or module unload can observe it.
        constexpr std::size_t maximumDestinationLength = 32768;
        std::wstring effectiveDestination(maximumDestinationLength, L'\0');
        std::size_t destinationLength{};
        const intptr_t result = callGuarded(function, &info, effectiveDestination.data(), effectiveDestination.size(),
                                            destinationLength, crashed);
        const auto outcome = core::extractionOutcome(crashed, result);
        if (!outcome || info.DestPath == nullptr) {
            return std::unexpected(outcome ? core::Error::ForeignCallFailed : outcome.error());
        }
        // PluginManager::GetFiles copies a plugin-rewritten DestPath back to its caller; copy it before unloading
        // the plugin module, while keeping every plugin-owned dereference under SEH
        // (Far source: far/plugins.cpp, PluginManager::GetFiles).
        effectiveDestination.resize(destinationLength);
        return effectiveDestination;
    }

} // namespace burlak::adapters::far_api
