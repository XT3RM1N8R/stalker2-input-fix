import os

exports = [
    "GetFileVersionInfoA", "GetFileVersionInfoByHandle", "GetFileVersionInfoExA",
    "GetFileVersionInfoExW", "GetFileVersionInfoSizeA", "GetFileVersionInfoSizeExA",
    "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW", "GetFileVersionInfoW",
    "VerFindFileA", "VerFindFileW", "VerInstallFileA", "VerInstallFileW",
    "VerLanguageNameA", "VerLanguageNameW", "VerQueryValueA", "VerQueryValueW"
]

with open("version_pointers.h", "w") as f:
    f.write('#pragma once\n')
    f.write('#include <windows.h>\n')
    f.write('extern "C" {\n')
    for exp in exports:
        f.write(f'    FARPROC ptr_{exp} = nullptr;\n')
    f.write('}\n\n')
    
    f.write('void LoadRealVersion() {\n')
    f.write('    char path[MAX_PATH];\n')
    f.write('    if (GetSystemDirectoryA(path, MAX_PATH)) {\n')
    f.write('        strcat_s(path, sizeof(path), "\\\\version.dll");\n')
    f.write('        HMODULE hReal = LoadLibraryA(path);\n')
    f.write('        if (hReal) {\n')
    for exp in exports:
        f.write(f'            ptr_{exp} = GetProcAddress(hReal, "{exp}");\n')
    f.write('        }\n')
    f.write('    }\n')
    f.write('}\n')

with open("version_asm.asm", "w") as f:
    f.write('.code\n')
    for exp in exports:
        f.write(f'extern ptr_{exp}:qword\n')
    f.write('\n')
    for exp in exports:
        f.write(f'{exp} proc\n')
        f.write(f'    jmp qword ptr [ptr_{exp}]\n')
        f.write(f'{exp} endp\n')
    f.write('end\n')

with open("version.def", "w") as f:
    f.write('LIBRARY version\n')
    f.write('EXPORTS\n')
    for exp in exports:
        f.write(f'    {exp}\n')
