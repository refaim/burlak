#include <windows.h>

#include <plugin.hpp>

#include <string>
#include <string_view>

namespace
{

    std::wstring rewrittenDestination;
    std::wstring overlongDestination;

    bool faithful(const GetFilesInfo &info)
    {
        if (info.Instance != reinterpret_cast<void *>(43)) {
            return false;
        }
        const auto &item = info.PanelItem[0];
        return info.StructSize == sizeof(info) && info.hPanel == reinterpret_cast<HANDLE>(7) && info.ItemsNumber == 3 &&
               info.Move == FALSE && info.OpMode == OPM_SILENT && item.FileName != nullptr &&
               std::wstring_view{item.FileName} == L"one.txt" && item.CreationTime.dwLowDateTime == 1 &&
               item.CreationTime.dwHighDateTime == 2 && item.LastAccessTime.dwLowDateTime == 3 &&
               item.LastAccessTime.dwHighDateTime == 4 && item.LastWriteTime.dwLowDateTime == 5 &&
               item.LastWriteTime.dwHighDateTime == 6 && item.ChangeTime.dwLowDateTime == 7 &&
               item.ChangeTime.dwHighDateTime == 8 && item.FileSize == 19 && item.AllocationSize == 31 &&
               item.AlternateFileName != nullptr && std::wstring_view{item.AlternateFileName} == L"ONE.TXT" &&
               item.Description != nullptr && std::wstring_view{item.Description} == L"description" &&
               item.Owner != nullptr && std::wstring_view{item.Owner} == L"owner" && item.CustomColumnData != nullptr &&
               item.CustomColumnNumber == 3 && std::wstring_view{item.CustomColumnData[0]} == L"column-a" &&
               std::wstring_view{item.CustomColumnData[1]} == L"column-b" && item.CustomColumnData[2] == nullptr &&
               item.Flags == (PPIF_SELECTED | PPIF_PROCESSDESCR) &&
               item.UserData.Data == reinterpret_cast<void *>(23) && item.UserData.FreeData != nullptr &&
               item.FileAttributes == (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM) && item.NumberOfLinks == 5 &&
               item.CRC32 == 0xabcdef && item.Reserved[0] == 29 && item.Reserved[1] == 31 &&
               info.PanelItem[2].FileAttributes == FILE_ATTRIBUTE_DIRECTORY && info.PanelItem[2].Flags == PPIF_SELECTED;
    }

} // namespace

extern "C" intptr_t WINAPI GetFilesW(GetFilesInfo *info)
{
    if (info == nullptr || info->DestPath == nullptr || info->PanelItem == nullptr) {
        return 0;
    }
    if (info->Instance == reinterpret_cast<void *>(13)) {
        return 0;
    }
    const bool expectedInvocation = info->PanelItem != nullptr && info->ItemsNumber != 0 &&
                                    info->PanelItem[0].FileName != nullptr &&
                                    std::wstring_view{info->PanelItem[0].FileName} == L"one.txt";
    if (expectedInvocation && !faithful(*info)) {
        return 0;
    }
    if (info->Instance == reinterpret_cast<void *>(45)) {
        rewrittenDestination = std::wstring{info->DestPath} + L"-rewritten";
        info->DestPath = rewrittenDestination.c_str();
    }
    if (info->Instance == reinterpret_cast<void *>(46)) {
        info->DestPath = nullptr;
        return 1;
    }
    if (info->Instance == reinterpret_cast<void *>(47)) {
        info->DestPath = reinterpret_cast<const wchar_t *>(1);
        return 1;
    }
    if (info->Instance == reinterpret_cast<void *>(48)) {
        overlongDestination.assign(32'768, L'x');
        info->DestPath = overlongDestination.c_str();
        return 1;
    }
    static_cast<void>(CreateDirectoryW(info->DestPath, nullptr));
    for (std::size_t index = 0; index < info->ItemsNumber; ++index) {
        if (info->PanelItem[index].FileName == nullptr) {
            return 0;
        }
        const std::wstring path = std::wstring{info->DestPath} + L"\\" + info->PanelItem[index].FileName;
        if ((info->PanelItem[index].FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
                return 0;
            }
            continue;
        }
        const HANDLE file =
            CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return 0;
        }
        if (info->Instance == reinterpret_cast<void *>(44)) {
            constexpr char contents[] = "content";
            DWORD written{};
            if (WriteFile(file, contents, sizeof(contents) - 1, &written, nullptr) == FALSE ||
                written != sizeof(contents) - 1) {
                CloseHandle(file);
                return 0;
            }
        }
        CloseHandle(file);
    }
    if (info->Instance == reinterpret_cast<void *>(44)) {
        const std::wstring marker = std::wstring{info->DestPath} + L"\\plugin-e2e.extracted";
        const HANDLE file =
            CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return 0;
        }
        CloseHandle(file);
    }
    return info->Instance == reinterpret_cast<void *>(2) ? 2 : 1;
}
