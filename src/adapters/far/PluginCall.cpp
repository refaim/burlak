#include "adapters/far/PluginCall.hpp"

#include "core/Policies.hpp"

#include <windows.h>

#include <plugin.hpp>

#include <bit>
#include <memory>
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

        intptr_t callGuarded(GetFilesFunction function, GetFilesInfo *info, bool &crashed) noexcept
        {
            intptr_t result{};
            __try {
                result = function(info);
            } __except (markCrash(crashed)) {
            }
            return result;
        }

    } // namespace

    std::expected<void, core::Error> callPluginGetFiles(core::PanelHandle panel, std::span<const core::Item> items,
                                                        const core::PluginModule &module, std::wstring_view destination)
    {
        UniqueModule loaded{LoadLibraryW(module.path.c_str())};
        if (!loaded) {
            return std::unexpected(core::Error::ForeignCallFailed);
        }

        const auto function = std::bit_cast<GetFilesFunction>(GetProcAddress(loaded.get(), "GetFilesW"));
        if (function == nullptr) {
            return std::unexpected(core::Error::ForeignCallFailed);
        }

        std::vector<PluginPanelItem> foreignItems(items.size());
        for (std::size_t index = 0; index < items.size(); ++index) {
            foreignItems[index].FileName = items[index].name.c_str();
            foreignItems[index].FileSize = items[index].size;
            foreignItems[index].FileAttributes =
                items[index].directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            foreignItems[index].UserData.Data = reinterpret_cast<void *>(items[index].userData.value);
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
        const intptr_t result = callGuarded(function, &info, crashed);
        return core::extractionOutcome(crashed, result);
    }

} // namespace burlak::adapters::far_api
