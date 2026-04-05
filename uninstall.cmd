@echo off
setlocal

taskkill /im ExplorerNavHook.exe /f >nul 2>&1

reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" ^
 /v ExplorerNavHook ^
 /f >nul 2>&1

echo Unregistered and stopped:
echo ExplorerNavHook.exe
pause