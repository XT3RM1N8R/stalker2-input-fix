@echo off
cd /d "%~dp0\.."
echo Compiling version.dll Global Proxy DLL with MASM...

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64

if not exist build mkdir build

ml64.exe /c /Cx /Fobuild\version_asm.obj src\version_asm.asm
if %errorlevel% neq 0 (
    echo MASM Compilation failed!
    exit /b %errorlevel%
)

cl.exe /nologo /O2 /W3 /MT /D "WIN32" /D "NDEBUG" /D "_WINDOWS" /D "_USRDLL" /I "include" /I "minhook" /Fobuild\ /c src\version_proxy.cpp minhook\hook.c minhook\trampoline.c minhook\hde\hde64.c minhook\hde\hde32.c minhook\buffer.c
if %errorlevel% neq 0 (
    echo C++ Compilation failed!
    exit /b %errorlevel%
)

link.exe /nologo /dll /def:src\version.def /out:version.dll /implib:build\version.lib user32.lib build\version_asm.obj build\version_proxy.obj build\hook.obj build\trampoline.obj build\hde64.obj build\hde32.obj build\buffer.obj
if %errorlevel% neq 0 (
    echo Linker failed!
    exit /b %errorlevel%
)

echo Compilation successful! version.dll generated.
