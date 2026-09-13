#pragma once

#include "core/Interfaces.hpp"

#include <cstdint>
#include <filesystem>

namespace burlak::adapters::win
{

    using ProcessProbe = bool (*)(std::uint32_t process);

    class Files final : public core::IFiles
    {
      public:
        Files();
        Files(std::filesystem::path root, std::uint32_t process, ProcessProbe processProbe);

        [[nodiscard]] std::expected<std::wstring, core::Error> runDirectory() override;
        [[nodiscard]] std::expected<std::wstring, core::Error> placeholder(std::wstring_view name,
                                                                           bool directory) override;
        [[nodiscard]] bool sameName(std::wstring_view left, std::wstring_view right) const override;
        [[nodiscard]] std::expected<void, core::Error> removeTree(std::wstring_view path) override;
        void sweep() override;

      private:
        std::filesystem::path root_;
        std::filesystem::path currentDirectory_;
        std::uint32_t process_{};
        std::uint64_t sequence_{};
        ProcessProbe processProbe_{};
    };

    [[nodiscard]] bool processAlive(std::uint32_t process);

} // namespace burlak::adapters::win
