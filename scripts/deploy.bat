@echo off
setlocal
cd /d "%~dp0\.."

if not exist "config.ini" (
    echo ERROR: config.ini not found!
    echo Please copy config.ini.example to config.ini and configure your paths.
    exit /b 1
)

for /f "usebackq tokens=1,* delims==" %%A in ("config.ini") do (
    if "%%A"=="GAME_PATH" set "GAME_PATH=%%B"
)

echo Deploying version.dll to game folder...
copy /Y "version.dll" "%GAME_PATH%\version.dll"
if %errorlevel% neq 0 (
    echo Deployment failed! Please make sure the game is closed.
    exit /b %errorlevel%
)
echo Deployment successful!
