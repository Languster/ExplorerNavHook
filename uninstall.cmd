@echo off
setlocal EnableExtensions

taskkill /im ExplorerNavHook.exe /f >nul 2>&1

rem Удаляем задачу Планировщика
schtasks /Delete /TN "ExplorerNavHook" /F >nul 2>&1

rem На всякий случай убираем и старую запись автозагрузки через Run
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v ExplorerNavHook /f >nul 2>&1

echo Unregistered and stopped:
echo ExplorerNavHook.exe
echo.
echo Scheduled Task removed:
echo ExplorerNavHook
pause
