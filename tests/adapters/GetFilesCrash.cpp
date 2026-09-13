#include <windows.h>

#include <plugin.hpp>

extern "C" intptr_t WINAPI GetFilesW(GetFilesInfo *)
{
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    return 0;
}
