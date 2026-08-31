.code
extern ptr_GetFileVersionInfoA:qword
extern ptr_GetFileVersionInfoByHandle:qword
extern ptr_GetFileVersionInfoExA:qword
extern ptr_GetFileVersionInfoExW:qword
extern ptr_GetFileVersionInfoSizeA:qword
extern ptr_GetFileVersionInfoSizeExA:qword
extern ptr_GetFileVersionInfoSizeExW:qword
extern ptr_GetFileVersionInfoSizeW:qword
extern ptr_GetFileVersionInfoW:qword
extern ptr_VerFindFileA:qword
extern ptr_VerFindFileW:qword
extern ptr_VerInstallFileA:qword
extern ptr_VerInstallFileW:qword
extern ptr_VerLanguageNameA:qword
extern ptr_VerLanguageNameW:qword
extern ptr_VerQueryValueA:qword
extern ptr_VerQueryValueW:qword

GetFileVersionInfoA proc
    jmp qword ptr [ptr_GetFileVersionInfoA]
GetFileVersionInfoA endp
GetFileVersionInfoByHandle proc
    jmp qword ptr [ptr_GetFileVersionInfoByHandle]
GetFileVersionInfoByHandle endp
GetFileVersionInfoExA proc
    jmp qword ptr [ptr_GetFileVersionInfoExA]
GetFileVersionInfoExA endp
GetFileVersionInfoExW proc
    jmp qword ptr [ptr_GetFileVersionInfoExW]
GetFileVersionInfoExW endp
GetFileVersionInfoSizeA proc
    jmp qword ptr [ptr_GetFileVersionInfoSizeA]
GetFileVersionInfoSizeA endp
GetFileVersionInfoSizeExA proc
    jmp qword ptr [ptr_GetFileVersionInfoSizeExA]
GetFileVersionInfoSizeExA endp
GetFileVersionInfoSizeExW proc
    jmp qword ptr [ptr_GetFileVersionInfoSizeExW]
GetFileVersionInfoSizeExW endp
GetFileVersionInfoSizeW proc
    jmp qword ptr [ptr_GetFileVersionInfoSizeW]
GetFileVersionInfoSizeW endp
GetFileVersionInfoW proc
    jmp qword ptr [ptr_GetFileVersionInfoW]
GetFileVersionInfoW endp
VerFindFileA proc
    jmp qword ptr [ptr_VerFindFileA]
VerFindFileA endp
VerFindFileW proc
    jmp qword ptr [ptr_VerFindFileW]
VerFindFileW endp
VerInstallFileA proc
    jmp qword ptr [ptr_VerInstallFileA]
VerInstallFileA endp
VerInstallFileW proc
    jmp qword ptr [ptr_VerInstallFileW]
VerInstallFileW endp
VerLanguageNameA proc
    jmp qword ptr [ptr_VerLanguageNameA]
VerLanguageNameA endp
VerLanguageNameW proc
    jmp qword ptr [ptr_VerLanguageNameW]
VerLanguageNameW endp
VerQueryValueA proc
    jmp qword ptr [ptr_VerQueryValueA]
VerQueryValueA endp
VerQueryValueW proc
    jmp qword ptr [ptr_VerQueryValueW]
VerQueryValueW endp
end
