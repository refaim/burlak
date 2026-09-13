#include "adapters/win/Files.hpp"

#include <doctest/doctest.h>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <vector>

namespace burlak::adapters::win
{

    namespace
    {

        std::vector<std::uint32_t> checkedProcesses;

        bool fakeProcessAlive(std::uint32_t process)
        {
            checkedProcesses.push_back(process);
            return process == 701;
        }

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

        std::filesystem::path testRoot()
        {
            return std::filesystem::temp_directory_path() / (L"burlak-files-" + std::to_wstring(GetCurrentProcessId()));
        }

    } // namespace

    TEST_SUITE("files adapter")
    {
        TEST_CASE("run directories and file or directory placeholders use a real temporary tree")
        {
            DirectoryGuard root{testRoot()};
            Files files{root.path(), 700, fakeProcessAlive};

            CHECK(files.placeholder(L"before.txt", false) == std::unexpected(core::Error::Unavailable));
            const auto first = files.runDirectory();
            REQUIRE(first.has_value());
            CHECK(std::filesystem::path{*first}.filename() == L"700-1");

            const auto file = files.placeholder(L"empty.txt", false);
            REQUIRE(file.has_value());
            CHECK(std::filesystem::is_regular_file(*file));
            CHECK(std::filesystem::file_size(*file) == 0);
            const auto directory = files.placeholder(L"folder", true);
            REQUIRE(directory.has_value());
            CHECK(std::filesystem::is_directory(*directory));
            CHECK(files.placeholder(L"", false) == std::unexpected(core::Error::Unavailable));
            CHECK(files.placeholder(L"nested\\escape.txt", false) == std::unexpected(core::Error::Unavailable));
            CHECK(files.placeholder(L"empty.txt", false) == std::unexpected(core::Error::Unavailable));
            CHECK(files.placeholder(L"con", false) == std::unexpected(core::Error::Unavailable));
            CHECK(files.placeholder(L"con", true) == std::unexpected(core::Error::Unavailable));

            const auto second = files.runDirectory();
            REQUIRE(second.has_value());
            CHECK(std::filesystem::path{*second}.filename() == L"700-2");
            CHECK(files.removeTree(*first).has_value());
            CHECK_FALSE(std::filesystem::exists(*first));
            CHECK(files.removeTree(*first).has_value());
        }

        TEST_CASE("run-directory collisions and locked trees remain expected failures")
        {
            DirectoryGuard root{testRoot()};
            std::filesystem::create_directories(root.path() / L"700-1");
            Files files{root.path(), 700, fakeProcessAlive};
            CHECK(files.runDirectory() == std::unexpected(core::Error::Unavailable));

            const auto lockedDirectory = root.path() / L"locked";
            std::filesystem::create_directory(lockedDirectory);
            const auto lockedFile = lockedDirectory / L"held.txt";
            std::ofstream lockedStream{lockedFile};
            lockedStream.close();
            const HANDLE lock = CreateFileW(lockedFile.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                            FILE_ATTRIBUTE_NORMAL, nullptr);
            REQUIRE(lock != INVALID_HANDLE_VALUE);
            CHECK(files.removeTree(lockedDirectory.wstring()) == std::unexpected(core::Error::Unavailable));
            CloseHandle(lock);
        }

        TEST_CASE("sweep removes this process and dead owners but preserves live and unrelated runs")
        {
            DirectoryGuard root{testRoot()};
            std::filesystem::create_directories(root.path() / L"700-old");
            std::filesystem::create_directories(root.path() / L"701-old");
            std::filesystem::create_directories(root.path() / L"702-old");
            std::filesystem::create_directories(root.path() / L"noise");
            std::ofstream unrelatedFile{root.path() / L"703-file"};
            unrelatedFile.close();
            checkedProcesses.clear();

            Files files{root.path(), 700, fakeProcessAlive};
            files.sweep();

            CHECK_FALSE(std::filesystem::exists(root.path() / L"700-old"));
            CHECK(std::filesystem::exists(root.path() / L"701-old"));
            CHECK_FALSE(std::filesystem::exists(root.path() / L"702-old"));
            CHECK(std::filesystem::exists(root.path() / L"noise"));
            CHECK(std::filesystem::exists(root.path() / L"703-file"));
            CHECK(checkedProcesses == std::vector<std::uint32_t>{701, 702});
        }

        TEST_CASE("filesystem failures stay expected and an absent sweep root is harmless")
        {
            DirectoryGuard root{testRoot()};
            std::filesystem::create_directories(root.path().parent_path());
            std::ofstream blockedRoot{root.path()};
            blockedRoot.close();
            Files blocked{root.path(), 700, fakeProcessAlive};
            CHECK(blocked.runDirectory() == std::unexpected(core::Error::Unavailable));
            blocked.sweep();

            std::filesystem::remove(root.path());
            Files absent{root.path(), 700, fakeProcessAlive};
            absent.sweep();
        }

        TEST_CASE("the process probe recognizes this process and a nonexistent process")
        {
            CHECK(processAlive(GetCurrentProcessId()));
            CHECK_FALSE(processAlive(UINT32_MAX));

            wchar_t command[] = L"cmd.exe /c exit 0";
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION child{};
            REQUIRE(CreateProcessW(nullptr, command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                                   &startup, &child) != FALSE);
            REQUIRE(WaitForSingleObject(child.hProcess, 5000) == WAIT_OBJECT_0);
            CHECK_FALSE(processAlive(child.dwProcessId));
            CloseHandle(child.hThread);
            CloseHandle(child.hProcess);
        }
    }

} // namespace burlak::adapters::win
