#pragma once

#include "core/Types.hpp"

#include <expected>
#include <span>
#include <string_view>

namespace burlak::adapters::far_api
{

    [[nodiscard]] bool hasGetFilesExport(const core::PluginModule &module);

    [[nodiscard]] std::expected<std::wstring, core::Error> callPluginGetFiles(core::PanelHandle panel,
                                                                              std::span<const core::Item> items,
                                                                              const core::PluginModule &module,
                                                                              std::wstring_view destination);

} // namespace burlak::adapters::far_api
