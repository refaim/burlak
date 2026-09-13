#include <windows.h>

#include <plugin.hpp>

#include <string>

extern "C" intptr_t WINAPI GetFilesW(GetFilesInfo *info)
{
    if (info == nullptr || info->DestPath == nullptr) {
        return 0;
    }
    if (info->Instance == reinterpret_cast<void *>(13)) {
        return 0;
    }
    static_cast<void>(CreateDirectoryW(info->DestPath, nullptr));
    for (std::size_t index = 0; index < info->ItemsNumber; ++index) {
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
        CloseHandle(file);
    }
    return info->Instance == reinterpret_cast<void *>(2) ? 2 : 1;
}
