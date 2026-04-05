#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <set>
#include <vector>

static HANDLE g_hSingleInstanceMutex = nullptr;

static std::wstring GetOwnExePath()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

static std::wstring GetOwnDirectory()
{
    std::wstring path = GetOwnExePath();
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos)
        return L"";
    return path.substr(0, pos + 1);
}

static std::wstring GetLoaderLogPath()
{
    return GetOwnDirectory() + L"ExplorerNavLoader.log";
}

static void Log(const wchar_t* text)
{
    std::wstring logPath = GetLoaderLogPath();

    HANDLE h = CreateFileW(
        logPath.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (h != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(h, text, (DWORD)(lstrlenW(text) * sizeof(wchar_t)), &written, nullptr);
        WriteFile(h, L"\r\n", 4, &written, nullptr);
        CloseHandle(h);
    }
}

static void LogFormat1(const wchar_t* fmt, DWORD a)
{
    wchar_t buf[512] = {};
    wsprintfW(buf, fmt, a);
    Log(buf);
}

static std::vector<DWORD> FindAllExplorerPids()
{
    std::vector<DWORD> pids;

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE)
        return pids;

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(hSnap, &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0)
            {
                pids.push_back(pe.th32ProcessID);
            }
        } while (Process32NextW(hSnap, &pe));
    }

    CloseHandle(hSnap);
    return pids;
}

static bool IsProcessAlive(DWORD pid)
{
    HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!hProcess)
        return false;

    DWORD wait = WaitForSingleObject(hProcess, 0);
    CloseHandle(hProcess);

    return wait == WAIT_TIMEOUT;
}

static bool IsDllAlreadyLoaded(DWORD pid, const wchar_t* dllNameOnly)
{
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnap == INVALID_HANDLE_VALUE)
        return false;

    MODULEENTRY32W me = {};
    me.dwSize = sizeof(me);

    bool found = false;

    if (Module32FirstW(hSnap, &me))
    {
        do
        {
            if (_wcsicmp(me.szModule, dllNameOnly) == 0)
            {
                found = true;
                break;
            }
        } while (Module32NextW(hSnap, &me));
    }

    CloseHandle(hSnap);
    return found;
}

static bool InjectDllIntoProcess(DWORD pid, const wchar_t* dllFullPath)
{
    LogFormat1(L"[Loader] InjectDllIntoProcess start pid=%lu", pid);

    HANDLE hProcess = OpenProcess(
        PROCESS_CREATE_THREAD |
        PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE |
        PROCESS_VM_READ,
        FALSE,
        pid
    );

    if (!hProcess)
    {
        LogFormat1(L"[Loader] OpenProcess failed, gle=%lu", GetLastError());
        return false;
    }

    SIZE_T bytes = (wcslen(dllFullPath) + 1) * sizeof(wchar_t);

    LPVOID remoteMem = VirtualAllocEx(hProcess, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem)
    {
        LogFormat1(L"[Loader] VirtualAllocEx failed, gle=%lu", GetLastError());
        CloseHandle(hProcess);
        return false;
    }

    if (!WriteProcessMemory(hProcess, remoteMem, dllFullPath, bytes, nullptr))
    {
        LogFormat1(L"[Loader] WriteProcessMemory failed, gle=%lu", GetLastError());
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!hKernel32)
    {
        LogFormat1(L"[Loader] GetModuleHandleW failed, gle=%lu", GetLastError());
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    FARPROC pLoadLibraryW = GetProcAddress(hKernel32, "LoadLibraryW");
    if (!pLoadLibraryW)
    {
        LogFormat1(L"[Loader] GetProcAddress failed, gle=%lu", GetLastError());
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    HANDLE hThread = CreateRemoteThread(
        hProcess,
        nullptr,
        0,
        (LPTHREAD_START_ROUTINE)pLoadLibraryW,
        remoteMem,
        0,
        nullptr
    );

    if (!hThread)
    {
        LogFormat1(L"[Loader] CreateRemoteThread failed, gle=%lu", GetLastError());
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    WaitForSingleObject(hThread, 5000);

    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);

    CloseHandle(hThread);
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hProcess);

    LogFormat1(L"[Loader] remote LoadLibraryW exitCode=%lu", exitCode);

    return exitCode != 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    // Один экземпляр, чтобы автозапуск + ручной запуск не плодили копии
    g_hSingleInstanceMutex = CreateMutexW(nullptr, TRUE, L"ExplorerNavLoader_SingleInstance_Mutex");
    if (!g_hSingleInstanceMutex)
        return 1;

    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(g_hSingleInstanceMutex);
        g_hSingleInstanceMutex = nullptr;
        return 0;
    }

    std::wstring exePath = GetOwnExePath();
    std::wstring dir = GetOwnDirectory();
    std::wstring dllPath = dir + L"ExplorerNavHook.dll";

    DeleteFileW(GetLoaderLogPath().c_str());

    Log(L"[Loader] ===== START =====");
    Log((L"[Loader] exe path: " + exePath).c_str());
    Log((L"[Loader] dir path: " + dir).c_str());
    Log((L"[Loader] dll path: " + dllPath).c_str());

    DWORD attrs = GetFileAttributesW(dllPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES)
    {
        LogFormat1(L"[Loader] DLL not found, gle=%lu", GetLastError());

        if (g_hSingleInstanceMutex)
        {
            CloseHandle(g_hSingleInstanceMutex);
            g_hSingleInstanceMutex = nullptr;
        }

        return 1;
    }

    Log(L"[Loader] DLL found");
    Log(L"[Loader] entering main loop");

    std::set<DWORD> trackedPids;

    while (true)
    {
        std::vector<DWORD> explorerPids = FindAllExplorerPids();
        std::set<DWORD> currentPids(explorerPids.begin(), explorerPids.end());

        for (auto it = trackedPids.begin(); it != trackedPids.end(); )
        {
            if (currentPids.find(*it) == currentPids.end() || !IsProcessAlive(*it))
            {
                LogFormat1(L"[Loader] explorer pid removed from tracking: %lu", *it);
                it = trackedPids.erase(it);
            }
            else
            {
                ++it;
            }
        }

        for (DWORD pid : explorerPids)
        {
            if (trackedPids.find(pid) == trackedPids.end())
            {
                LogFormat1(L"[Loader] new explorer pid detected: %lu", pid);

                Sleep(300);

                if (!IsDllAlreadyLoaded(pid, L"ExplorerNavHook.dll"))
                {
                    LogFormat1(L"[Loader] DLL missing in pid=%lu, injecting...", pid);

                    if (InjectDllIntoProcess(pid, dllPath.c_str()))
                    {
                        LogFormat1(L"[Loader] injection succeeded for pid=%lu", pid);
                    }
                    else
                    {
                        LogFormat1(L"[Loader] injection failed for pid=%lu", pid);
                    }
                }
                else
                {
                    LogFormat1(L"[Loader] DLL already loaded in pid=%lu", pid);
                }

                trackedPids.insert(pid);
            }
            else
            {
                if (!IsDllAlreadyLoaded(pid, L"ExplorerNavHook.dll"))
                {
                    LogFormat1(L"[Loader] tracked pid=%lu lost DLL, retry inject", pid);

                    if (InjectDllIntoProcess(pid, dllPath.c_str()))
                    {
                        LogFormat1(L"[Loader] retry injection succeeded for pid=%lu", pid);
                    }
                    else
                    {
                        LogFormat1(L"[Loader] retry injection failed for pid=%lu", pid);
                    }
                }
            }
        }

        Sleep(1000);
    }

    // сюда код не дойдёт, но пусть будет аккуратно
    if (g_hSingleInstanceMutex)
    {
        CloseHandle(g_hSingleInstanceMutex);
        g_hSingleInstanceMutex = nullptr;
    }

    return 0;
}