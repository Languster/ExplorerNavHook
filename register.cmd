@echo off
setlocal EnableExtensions

set "APPDIR=%~dp0"
set "EXE=%APPDIR%ExplorerNavHook.exe"
set "TASKNAME=ExplorerNavHook"
for /f %%i in ('whoami') do set "CURUSER=%%i"

if not exist "%EXE%" (
    echo ERROR: not found:
    echo %EXE%
    pause
    exit /b 1
)

rem На всякий случай убираем старую запись Run, чтобы не было двойного запуска
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v ExplorerNavHook /f >nul 2>&1

rem Удаляем старую задачу, если уже существует
schtasks /Delete /TN "%TASKNAME%" /F >nul 2>&1

rem Создаем задачу Планировщика: запуск при входе пользователя, без задержки, с повышенными правами
schtasks /Create ^
 /TN "%TASKNAME%" ^
 /SC ONLOGON ^
 /TR "\"%EXE%\"" ^
 /RL HIGHEST ^
 /RU "%CURUSER%" ^
 /F

if errorlevel 1 (
    echo ERROR: failed to create scheduled task
    echo Try running this file as administrator.
    pause
    exit /b 1
)

rem Сразу запускаем программу
start "" "%EXE%"

echo Registered and started:
echo %EXE%
echo.
echo Scheduled Task created:
echo %TASKNAME%
pause
