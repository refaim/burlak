#include "adapters/win/Files.hpp"

#include <doctest/doctest.h>

#include <windows.h>

#include <chrono>
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

        std::filesystem::path fakeTempDirectory(std::error_code &error)
        {
            error.clear();
            return testRoot();
        }

        std::filesystem::path unavailableTempDirectory(std::error_code &error)
        {
            error = std::make_error_code(std::errc::permission_denied);
            return {};
        }

        std::filesystem::path emptyTempDirectory(std::error_code &error)
        {
            error.clear();
            return {};
        }

    } // namespace

    TEST_SUITE("files adapter")
    {
        TEST_CASE("run directories and file or directory placeholders use a real temporary tree")
        {
            DirectoryGuard root{testRoot()};
            Files files{root.path(), 700, fakeProcessAlive};
            CHECK(files.processAlive(701));
            CHECK_FALSE(files.processAlive(702));

            CHECK(files.placeholder(L"before.txt", false) == std::unexpected(core::Error::Unavailable));
            const auto first = files.runDirectory();
            REQUIRE(first.has_value());
            CHECK(std::filesystem::path{*first}.filename() == L"700-1");
            CHECK_FALSE(files.nameBefore(L"Report.txt", L"REPORT.TXT"));
            CHECK(files.nameBefore(L"one.txt", L"two.txt"));
            CHECK_FALSE(files.nameBefore(L"two.txt", L"one.txt"));

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

        TEST_CASE("run-directory collisions are skipped and locked trees remain expected failures")
        {
            DirectoryGuard root{testRoot()};
            std::filesystem::create_directories(root.path() / L"700-1");
            Files files{root.path(), 700, fakeProcessAlive};
            const auto directory = files.runDirectory();
            REQUIRE(directory.has_value());
            CHECK(std::filesystem::path{*directory}.filename() == L"700-2");

            std::ofstream occupiedFile{root.path() / L"800-1"};
            occupiedFile.close();
            Files blockedCandidate{root.path(), 800, fakeProcessAlive};
            CHECK(blockedCandidate.runDirectory() == std::unexpected(core::Error::Unavailable));

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

        TEST_CASE("sweep removes old own and dead-owner runs but keeps every young or live foreign run")
        {
            DirectoryGuard root{testRoot()};
            std::filesystem::create_directories(root.path() / L"700-old");
            std::filesystem::create_directories(root.path() / L"700-recent");
            std::filesystem::create_directories(root.path() / L"701-old");
            std::filesystem::create_directories(root.path() / L"702-old");
            std::filesystem::create_directories(root.path() / L"702-recent");
            std::filesystem::create_directories(root.path() / L"noise");
            std::ofstream unrelatedFile{root.path() / L"703-file"};
            unrelatedFile.close();
            const auto old = std::filesystem::file_time_type::clock::now() - std::chrono::minutes{11};
            std::filesystem::last_write_time(root.path() / L"700-old", old);
            std::filesystem::last_write_time(root.path() / L"701-old", old);
            std::filesystem::last_write_time(root.path() / L"702-old", old);
            checkedProcesses.clear();

            Files files{root.path(), 700, fakeProcessAlive};
            files.sweep();

            CHECK_FALSE(std::filesystem::exists(root.path() / L"700-old"));
            CHECK(std::filesystem::exists(root.path() / L"700-recent"));
            CHECK(std::filesystem::exists(root.path() / L"701-old"));
            CHECK_FALSE(std::filesystem::exists(root.path() / L"702-old"));
            CHECK(std::filesystem::exists(root.path() / L"702-recent"));
            CHECK(std::filesystem::exists(root.path() / L"noise"));
            CHECK(std::filesystem::exists(root.path() / L"703-file"));
            CHECK(checkedProcesses == std::vector<std::uint32_t>{701, 702, 702});
        }

        TEST_CASE("sweep keeps an old run while any regular file is open without sharing")
        {
            DirectoryGuard root{testRoot()};
            const auto run = root.path() / L"700-old";
            std::filesystem::create_directories(run / L"nested");
            const auto file = run / L"nested" / L"upload.bin";
            std::ofstream{file} << "payload";
            const auto old = std::filesystem::file_time_type::clock::now() - std::chrono::minutes{4};
            std::filesystem::last_write_time(run, old);
            const HANDLE held =
                CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            REQUIRE(held != INVALID_HANDLE_VALUE);
            Files files{root.path(), 700, fakeProcessAlive};

            files.sweep();
            CHECK(std::filesystem::exists(run));

            CloseHandle(held);
            files.sweep();
            CHECK_FALSE(std::filesystem::exists(run));
        }

        TEST_CASE("touch restarts a real run directory's grace clock")
        {
            DirectoryGuard root{testRoot()};
            const auto run = root.path() / L"700-old";
            std::filesystem::create_directories(run);
            const auto old = std::filesystem::file_time_type::clock::now() - std::chrono::minutes{11};
            std::filesystem::last_write_time(run, old);
            const auto before = std::filesystem::last_write_time(run);
            Files files{root.path(), 700, fakeProcessAlive};

            CHECK(files.touch(run.wstring()).has_value());
            CHECK(std::filesystem::last_write_time(run) > before);
            CHECK(files.touch((root.path() / L"missing").wstring()) == std::unexpected(core::Error::Unavailable));
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

        TEST_CASE("temporary-directory discovery never throws and its failure disables only run creation")
        {
            CHECK_NOTHROW(Files{});

            DirectoryGuard root{testRoot()};
            Files resolved{700, fakeProcessAlive, fakeTempDirectory};
            const auto directory = resolved.runDirectory();
            REQUIRE(directory.has_value());
            CHECK(std::filesystem::path{*directory}.parent_path() == root.path() / L"Burlak");

            Files unavailable{700, fakeProcessAlive, unavailableTempDirectory};
            CHECK(unavailable.runDirectory() == std::unexpected(core::Error::Unavailable));
            unavailable.sweep();

            Files empty{700, fakeProcessAlive, emptyTempDirectory};
            CHECK(empty.runDirectory() == std::unexpected(core::Error::Unavailable));
            empty.sweep();
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
