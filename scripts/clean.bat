@echo off
cd /d "%~dp0\.."
echo Cleaning build folder...
rmdir /s /q build 2>nul
echo Clean complete!
