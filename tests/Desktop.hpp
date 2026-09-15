#pragma once

#include <windows.h>

#include <cstdio>

namespace burlak::tests
{

    inline bool desktopAvailable(const char *unavailableReason)
    {
        wchar_t disabled[2]{};
        if (GetEnvironmentVariableW(L"BURLAK_NO_DESKTOP", disabled, 2) == 1 && disabled[0] == L'1') {
            std::fputs("SKIP: BURLAK_NO_DESKTOP\n", stderr);
            return false;
        }

        USEROBJECTFLAGS flags{};
        DWORD needed{};
        POINT cursor{};
        const bool available =
            GetUserObjectInformationW(GetProcessWindowStation(), UOI_FLAGS, &flags, sizeof(flags), &needed) != FALSE &&
            (flags.dwFlags & WSF_VISIBLE) != 0 && GetCursorPos(&cursor) != FALSE;
        if (!available) {
            std::fputs(unavailableReason, stderr);
        }
        return available;
    }

} // namespace burlak::tests
