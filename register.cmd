@echo off
setlocal

set "APPDIR=%~dp0"
set "EXE=%APPDIR%ExplorerNavHook.exe"

if not exist "%EXE%" (
    echo ERROR: not found:
    echo %EXE%
    pause
    exit /b 1
)

reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" ^
 /v ExplorerNavHook ^
 /t REG_SZ ^
 /d "\"%EXE%\"" ^
 /f >nul

start "" "%EXE%"

echo Registered and started:
echo %EXE%
pause