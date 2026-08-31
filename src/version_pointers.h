#pragma once
#include <windows.h>
extern "C" {
    FARPROC ptr_GetFileVersionInfoA = nullptr;
    FARPROC ptr_GetFileVersionInfoByHandle = nullptr;
    FARPROC ptr_GetFileVersionInfoExA = nullptr;
    FARPROC ptr_GetFileVersionInfoExW = nullptr;
    FARPROC ptr_GetFileVersionInfoSizeA = nullptr;
    FARPROC ptr_GetFileVersionInfoSizeExA = nullptr;
    FARPROC ptr_GetFileVersionInfoSizeExW = nullptr;
    FARPROC ptr_GetFileVersionInfoSizeW = nullptr;
    FARPROC ptr_GetFileVersionInfoW = nullptr;
    FARPROC ptr_VerFindFileA = nullptr;
    FARPROC ptr_VerFindFileW = nullptr;
    FARPROC ptr_VerInstallFileA = nullptr;
    FARPROC ptr_VerInstallFileW = nullptr;
    FARPROC ptr_VerLanguageNameA = nullptr;
    FARPROC ptr_VerLanguageNameW = nullptr;
    FARPROC ptr_VerQueryValueA = nullptr;
    FARPROC ptr_VerQueryValueW = nullptr;
}

void LoadRealVersion() {
    char path[MAX_PATH];
    if (GetSystemDirectoryA(path, MAX_PATH)) {
        strcat_s(path, sizeof(path), "\\version.dll");
        HMODULE hReal = LoadLibraryA(path);
        if (hReal) {
            ptr_GetFileVersionInfoA = GetProcAddress(hReal, "GetFileVersionInfoA");
            ptr_GetFileVersionInfoByHandle = GetProcAddress(hReal, "GetFileVersionInfoByHandle");
            ptr_GetFileVersionInfoExA = GetProcAddress(hReal, "GetFileVersionInfoExA");
            ptr_GetFileVersionInfoExW = GetProcAddress(hReal, "GetFileVersionInfoExW");
            ptr_GetFileVersionInfoSizeA = GetProcAddress(hReal, "GetFileVersionInfoSizeA");
            ptr_GetFileVersionInfoSizeExA = GetProcAddress(hReal, "GetFileVersionInfoSizeExA");
            ptr_GetFileVersionInfoSizeExW = GetProcAddress(hReal, "GetFileVersionInfoSizeExW");
            ptr_GetFileVersionInfoSizeW = GetProcAddress(hReal, "GetFileVersionInfoSizeW");
            ptr_GetFileVersionInfoW = GetProcAddress(hReal, "GetFileVersionInfoW");
            ptr_VerFindFileA = GetProcAddress(hReal, "VerFindFileA");
            ptr_VerFindFileW = GetProcAddress(hReal, "VerFindFileW");
            ptr_VerInstallFileA = GetProcAddress(hReal, "VerInstallFileA");
            ptr_VerInstallFileW = GetProcAddress(hReal, "VerInstallFileW");
            ptr_VerLanguageNameA = GetProcAddress(hReal, "VerLanguageNameA");
            ptr_VerLanguageNameW = GetProcAddress(hReal, "VerLanguageNameW");
            ptr_VerQueryValueA = GetProcAddress(hReal, "VerQueryValueA");
            ptr_VerQueryValueW = GetProcAddress(hReal, "VerQueryValueW");
        }
    }
}
