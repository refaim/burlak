#include "adapters/win/Files.hpp"

#include "core/Policies.hpp"

#include <windows.h>

#include <array>
#include <fstream>
#include <memory>
#include <system_error>

namespace burlak::adapters::win
{

    namespace
    {

        struct ProcessHandleCloser
        {
            void operator()(void *handle) const noexcept
            {
                static_cast<void>(CloseHandle(handle));
            }
        };

        using ProcessHandle = std::unique_ptr<void, ProcessHandleCloser>;

        [[nodiscard]] std::expected<std::wstring, core::Error> placeholderOutcome(bool created,
                                                                                  const std::filesystem::path &path)
        {
            const std::array<std::expected<std::wstring, core::Error>, 2> outcomes{
                std::unexpected(core::Error::Unavailable), path.wstring()};
            return outcomes[static_cast<std::size_t>(created)];
        }

        [[nodiscard]] std::filesystem::path systemTempDirectory(std::error_code &error)
        {
            return std::filesystem::temp_directory_path(error);
        }

        [[nodiscard]] std::expected<std::filesystem::path, core::Error> temporaryRoot(
            TempDirectoryProbe tempDirectoryProbe)
        {
            std::error_code error;
            const auto directory = tempDirectoryProbe(error);
            if (error || directory.empty()) {
                return std::unexpected(core::Error::Unavailable);
            }
            return directory / L"Burlak";
        }

    } // namespace

    Files::Files() : Files{GetCurrentProcessId(), processAlive, systemTempDirectory}
    {
    }

    Files::Files(std::filesystem::path root, std::uint32_t process, ProcessProbe processProbe)
        : root_{std::move(root)}, process_{process}, processProbe_{processProbe}
    {
    }

    Files::Files(std::uint32_t process, ProcessProbe processProbe, TempDirectoryProbe tempDirectoryProbe)
        : Files{temporaryRoot(tempDirectoryProbe).value_or(std::filesystem::path{}), process, processProbe}
    {
    }

    std::expected<std::wstring, core::Error> Files::runDirectory()
    {
        if (root_.empty()) {
            return std::unexpected(core::Error::Unavailable);
        }
        std::error_code error;
        static_cast<void>(std::filesystem::create_directories(root_, error));
        if (error) {
            return std::unexpected(core::Error::Unavailable);
        }

        for (;;) {
            currentDirectory_ = root_ / (std::to_wstring(process_) + L"-" + std::to_wstring(++sequence_));
            error.clear();
            if (std::filesystem::create_directory(currentDirectory_, error)) {
                return currentDirectory_.wstring();
            }
            if (error) {
                return std::unexpected(core::Error::Unavailable);
            }
        }
    }

    std::expected<std::wstring, core::Error> Files::placeholder(std::wstring_view name, bool directory)
    {
        const std::filesystem::path relative{name};
        if (currentDirectory_.empty() || relative.empty() || relative.filename() != relative) {
            return std::unexpected(core::Error::Unavailable);
        }
        const auto path = currentDirectory_ / relative;
        std::error_code error;
        if (std::filesystem::exists(path, error) || error) {
            return std::unexpected(core::Error::Unavailable);
        }
        if (directory) {
            return placeholderOutcome(std::filesystem::create_directory(path, error), path);
        }
        std::ofstream stream{path, std::ios::binary};
        return placeholderOutcome(static_cast<bool>(stream), path);
    }

    bool Files::nameBefore(std::wstring_view left, std::wstring_view right) const
    {
        return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                    static_cast<int>(right.size()), TRUE) == CSTR_LESS_THAN;
    }

    std::expected<void, core::Error> Files::removeTree(std::wstring_view path)
    {
        std::error_code error;
        static_cast<void>(std::filesystem::remove_all(std::filesystem::path{path}, error));
        return core::expectedOutcome(!error, core::Error::Unavailable);
    }

    void Files::sweep()
    {
        if (root_.empty()) {
            return;
        }
        std::error_code error;
        std::filesystem::directory_iterator entry{root_, error};
        if (error) {
            return;
        }
        const std::filesystem::directory_iterator end;
        for (; entry != end; entry.increment(error)) {
            if (!entry->is_directory(error)) {
                error.clear();
                continue;
            }
            const auto owner = core::runOwner(entry->path().filename().wstring());
            if (!owner) {
                continue;
            }
            const bool alive = *owner == process_ || processProbe_(*owner);
            if (core::shouldSweepRun(entry->path().filename().wstring(), process_, alive)) {
                static_cast<void>(std::filesystem::remove_all(entry->path(), error));
                error.clear();
            }
        }
    }

    bool processAlive(std::uint32_t process)
    {
        ProcessHandle handle{OpenProcess(SYNCHRONIZE, FALSE, process)};
        if (!handle) {
            return GetLastError() != ERROR_INVALID_PARAMETER;
        }
        return WaitForSingleObject(handle.get(), 0) == WAIT_TIMEOUT;
    }

} // namespace burlak::adapters::win
