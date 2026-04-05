#include "pch.h"
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")

#define DEFAULT_TARGET_INDENT          25
#define DEFAULT_REMOVE_HASBUTTONS      1
#define DEFAULT_REMOVE_HASLINES        1
#define DEFAULT_REMOVE_LINESATROOT     1
#define DEFAULT_ENABLE_LOG             1

// Точечный repatch только при смене tree hwnd внутри того же окна Explorer
#define TABMODE_REPATCH_DELAY1_MS      120
#define TABMODE_REPATCH_DELAY2_MS      320

static HMODULE g_hModule = nullptr;
static HANDLE g_hWorkerThread = nullptr;
static HWINEVENTHOOK g_hEventHookShow = nullptr;
static HWINEVENTHOOK g_hEventHookCreate = nullptr;

static int  g_TargetIndent = DEFAULT_TARGET_INDENT;
static BOOL g_RemoveHasButtons = DEFAULT_REMOVE_HASBUTTONS;
static BOOL g_RemoveHasLines = DEFAULT_REMOVE_HASLINES;
static BOOL g_RemoveLinesAtRoot = DEFAULT_REMOVE_LINESATROOT;
static BOOL g_EnableLog = DEFAULT_ENABLE_LOG;

static CRITICAL_SECTION g_StateLock;

struct ExplorerState
{
    HWND hExplorer;
    HWND hTree;
    bool repatchPending;
};

static std::vector<ExplorerState> g_States;

// =========================
// Лог
// =========================
static void Log(const wchar_t* text)
{
    if (!g_EnableLog)
        return;

    wchar_t path[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, path);
    wcscat_s(path, L"ExplorerNavHook.log");

    HANDLE h = CreateFileW(
        path,
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

static void LogFmt(const wchar_t* fmt, UINT_PTR a = 0, UINT_PTR b = 0)
{
    wchar_t buf[512] = {};
    wsprintfW(buf, fmt, a, b);
    Log(buf);
}

// =========================
// INI
// =========================
static void LoadSettings()
{
    wchar_t dllPath[MAX_PATH] = {};
    GetModuleFileNameW(g_hModule, dllPath, MAX_PATH);

    wchar_t* slash = wcsrchr(dllPath, L'\\');
    if (!slash)
        return;

    *(slash + 1) = L'\0';

    wchar_t iniPath[MAX_PATH] = {};
    wcscpy_s(iniPath, dllPath);
    wcscat_s(iniPath, L"ExplorerNavHook.ini");

    g_TargetIndent = GetPrivateProfileIntW(L"Hook", L"TargetIndent", DEFAULT_TARGET_INDENT, iniPath);
    g_RemoveHasButtons = GetPrivateProfileIntW(L"Hook", L"RemoveHasButtons", DEFAULT_REMOVE_HASBUTTONS, iniPath);
    g_RemoveHasLines = GetPrivateProfileIntW(L"Hook", L"RemoveHasLines", DEFAULT_REMOVE_HASLINES, iniPath);
    g_RemoveLinesAtRoot = GetPrivateProfileIntW(L"Hook", L"RemoveLinesAtRoot", DEFAULT_REMOVE_LINESATROOT, iniPath);
    g_EnableLog = GetPrivateProfileIntW(L"Hook", L"EnableLog", DEFAULT_ENABLE_LOG, iniPath);

    wchar_t buf[256];
    wsprintfW(
        buf,
        L"[ExplorerNavHook] settings loaded: indent=%d buttons=%d lines=%d root=%d log=%d",
        g_TargetIndent,
        g_RemoveHasButtons,
        g_RemoveHasLines,
        g_RemoveLinesAtRoot,
        g_EnableLog
    );
    Log(buf);
}

// =========================
// Проверки
// =========================
static bool IsExplorerTopWindow(HWND hwnd)
{
    if (!IsWindow(hwnd))
        return false;

    wchar_t cls[128] = {};
    GetClassNameW(hwnd, cls, 128);

    return (lstrcmpiW(cls, L"CabinetWClass") == 0 ||
        lstrcmpiW(cls, L"ExploreWClass") == 0);
}

static HWND FindExplorerTopWindowFromChild(HWND hwnd)
{
    HWND cur = hwnd;

    for (int i = 0; i < 20 && cur; ++i)
    {
        if (IsExplorerTopWindow(cur))
            return cur;

        cur = GetParent(cur);
    }

    return nullptr;
}

static bool IsLikelyNavTree(HWND hwnd)
{
    if (!IsWindow(hwnd))
        return false;

    wchar_t cls[128] = {};
    GetClassNameW(hwnd, cls, 128);

    if (lstrcmpiW(cls, WC_TREEVIEWW) != 0 && lstrcmpiW(cls, L"SysTreeView32") != 0)
        return false;

    RECT rc{};
    if (!GetWindowRect(hwnd, &rc))
        return false;

    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    if (w < 120 || h < 200)
        return false;

    return FindExplorerTopWindowFromChild(hwnd) != nullptr;
}

// =========================
// Патч дерева
// =========================
static void PatchTree(HWND hTree)
{
    if (!IsWindow(hTree))
        return;

    LONG_PTR style = GetWindowLongPtrW(hTree, GWL_STYLE);

    if (g_RemoveHasButtons)
        style &= ~TVS_HASBUTTONS;

    if (g_RemoveHasLines)
        style &= ~TVS_HASLINES;

    if (g_RemoveLinesAtRoot)
        style &= ~TVS_LINESATROOT;

    SetWindowLongPtrW(hTree, GWL_STYLE, style);

    SendMessageW(hTree, TVM_SETINDENT, (WPARAM)g_TargetIndent, 0);

    SetWindowPos(
        hTree,
        nullptr,
        0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED
    );

    InvalidateRect(hTree, nullptr, TRUE);
    UpdateWindow(hTree);

    LogFmt(L"[ExplorerNavHook] patched tree hwnd=%p", (UINT_PTR)hTree);
}

static BOOL CALLBACK FindTreeEnumProc(HWND hwnd, LPARAM lParam)
{
    HWND* pFound = (HWND*)lParam;
    if (*pFound)
        return FALSE;

    if (IsLikelyNavTree(hwnd))
    {
        *pFound = hwnd;
        return FALSE;
    }

    EnumChildWindows(hwnd, FindTreeEnumProc, lParam);
    return TRUE;
}

static HWND FindNavTreeInExplorer(HWND hExplorer)
{
    if (!IsExplorerTopWindow(hExplorer))
        return nullptr;

    HWND found = nullptr;
    EnumChildWindows(hExplorer, FindTreeEnumProc, (LPARAM)&found);
    return found;
}

static void PatchExplorerWindow(HWND hExplorer)
{
    HWND hTree = FindNavTreeInExplorer(hExplorer);
    if (hTree)
        PatchTree(hTree);
}

// =========================
// Состояние окон Explorer
// =========================
static ExplorerState* GetOrCreateState_NoLock(HWND hExplorer)
{
    for (auto& s : g_States)
    {
        if (s.hExplorer == hExplorer)
            return &s;
    }

    ExplorerState s{};
    s.hExplorer = hExplorer;
    s.hTree = nullptr;
    s.repatchPending = false;
    g_States.push_back(s);
    return &g_States.back();
}

static void CleanupDeadStates()
{
    EnterCriticalSection(&g_StateLock);

    for (size_t i = 0; i < g_States.size(); )
    {
        if (!IsWindow(g_States[i].hExplorer))
            g_States.erase(g_States.begin() + i);
        else
            ++i;
    }

    LeaveCriticalSection(&g_StateLock);
}

// =========================
// Точечный repatch при смене tree hwnd
// =========================
struct RepatchContext
{
    HWND hExplorer;
};

static DWORD WINAPI RepatchThreadProc(LPVOID param)
{
    RepatchContext* ctx = (RepatchContext*)param;
    if (!ctx)
        return 0;

    HWND hExplorer = ctx->hExplorer;
    delete ctx;

    Sleep(TABMODE_REPATCH_DELAY1_MS);
    if (IsWindow(hExplorer))
    {
        PatchExplorerWindow(hExplorer);
        Log(L"[ExplorerNavHook] tabmode repatch #1 done");
    }

    Sleep(TABMODE_REPATCH_DELAY2_MS - TABMODE_REPATCH_DELAY1_MS);
    if (IsWindow(hExplorer))
    {
        PatchExplorerWindow(hExplorer);
        Log(L"[ExplorerNavHook] tabmode repatch #2 done");
    }

    EnterCriticalSection(&g_StateLock);
    for (auto& s : g_States)
    {
        if (s.hExplorer == hExplorer)
        {
            s.repatchPending = false;
            break;
        }
    }
    LeaveCriticalSection(&g_StateLock);

    return 0;
}

static void ScheduleTabModeRepatch(HWND hExplorer)
{
    if (!IsWindow(hExplorer))
        return;

    bool shouldStart = false;

    EnterCriticalSection(&g_StateLock);
    ExplorerState* s = GetOrCreateState_NoLock(hExplorer);
    if (s && !s->repatchPending)
    {
        s->repatchPending = true;
        shouldStart = true;
    }
    LeaveCriticalSection(&g_StateLock);

    if (!shouldStart)
        return;

    RepatchContext* ctx = new RepatchContext{};
    ctx->hExplorer = hExplorer;

    HANDLE hThread = CreateThread(nullptr, 0, RepatchThreadProc, ctx, 0, nullptr);
    if (hThread)
    {
        CloseHandle(hThread);
        Log(L"[ExplorerNavHook] tabmode repatch scheduled");
    }
    else
    {
        EnterCriticalSection(&g_StateLock);
        for (auto& s : g_States)
        {
            if (s.hExplorer == hExplorer)
            {
                s.repatchPending = false;
                break;
            }
        }
        LeaveCriticalSection(&g_StateLock);

        delete ctx;
    }
}

static void UpdateExplorerStateAndMaybeRepatch(HWND hExplorer)
{
    if (!IsExplorerTopWindow(hExplorer))
        return;

    HWND currentTree = FindNavTreeInExplorer(hExplorer);
    if (!currentTree)
        return;

    bool needRepatch = false;
    HWND oldTree = nullptr;

    EnterCriticalSection(&g_StateLock);

    ExplorerState* s = GetOrCreateState_NoLock(hExplorer);
    if (s)
    {
        oldTree = s->hTree;

        if (s->hTree == nullptr)
        {
            s->hTree = currentTree;
        }
        else if (s->hTree != currentTree)
        {
            s->hTree = currentTree;
            needRepatch = true;
        }
    }

    LeaveCriticalSection(&g_StateLock);

    PatchTree(currentTree);

    if (needRepatch)
    {
        LogFmt(L"[ExplorerNavHook] tree hwnd changed old=%p new=%p", (UINT_PTR)oldTree, (UINT_PTR)currentTree);
        ScheduleTabModeRepatch(hExplorer);
    }
}

// =========================
// Initial pass
// =========================
static BOOL CALLBACK EnumTopWindowsProc(HWND hwnd, LPARAM)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    if (pid == GetCurrentProcessId() && IsExplorerTopWindow(hwnd))
    {
        UpdateExplorerStateAndMaybeRepatch(hwnd);
    }

    return TRUE;
}

static void InitialPatchAllExplorerWindows()
{
    EnumWindows(EnumTopWindowsProc, 0);
}

// =========================
// WinEvent callback
// =========================
static void CALLBACK WinEventProc(
    HWINEVENTHOOK,
    DWORD event,
    HWND hwnd,
    LONG idObject,
    LONG,
    DWORD,
    DWORD)
{
    if (!hwnd)
        return;

    if (idObject != OBJID_WINDOW)
        return;

    CleanupDeadStates();

    if (IsExplorerTopWindow(hwnd))
    {
        LogFmt(L"[ExplorerNavHook] top explorer event=%u hwnd=%p", event, (UINT_PTR)hwnd);
        UpdateExplorerStateAndMaybeRepatch(hwnd);
        return;
    }

    if (IsLikelyNavTree(hwnd))
    {
        HWND hExplorer = FindExplorerTopWindowFromChild(hwnd);
        if (hExplorer)
        {
            LogFmt(L"[ExplorerNavHook] nav tree event=%u hwnd=%p", event, (UINT_PTR)hwnd);
            UpdateExplorerStateAndMaybeRepatch(hExplorer);
        }
        else
        {
            PatchTree(hwnd);
        }
        return;
    }

    HWND hExplorer = FindExplorerTopWindowFromChild(hwnd);
    if (hExplorer)
    {
        UpdateExplorerStateAndMaybeRepatch(hExplorer);
    }
}

// =========================
// Поток с message loop
// =========================
static DWORD WINAPI WorkerThreadProc(LPVOID)
{
    InitializeCriticalSection(&g_StateLock);

    LoadSettings();
    Log(L"[ExplorerNavHook] worker started");

    InitialPatchAllExplorerWindows();

    DWORD pid = GetCurrentProcessId();

    g_hEventHookShow = SetWinEventHook(
        EVENT_OBJECT_SHOW,
        EVENT_OBJECT_SHOW,
        nullptr,
        WinEventProc,
        pid,
        0,
        WINEVENT_OUTOFCONTEXT
    );

    g_hEventHookCreate = SetWinEventHook(
        EVENT_OBJECT_CREATE,
        EVENT_OBJECT_CREATE,
        nullptr,
        WinEventProc,
        pid,
        0,
        WINEVENT_OUTOFCONTEXT
    );

    if (!g_hEventHookShow || !g_hEventHookCreate)
        Log(L"[ExplorerNavHook] failed to install WinEvent hook");
    else
        Log(L"[ExplorerNavHook] WinEvent hooks installed");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hEventHookShow)
    {
        UnhookWinEvent(g_hEventHookShow);
        g_hEventHookShow = nullptr;
    }

    if (g_hEventHookCreate)
    {
        UnhookWinEvent(g_hEventHookCreate);
        g_hEventHookCreate = nullptr;
    }

    DeleteCriticalSection(&g_StateLock);

    Log(L"[ExplorerNavHook] worker stopped");
    return 0;
}

// =========================
// DllMain
// =========================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        g_hWorkerThread = CreateThread(nullptr, 0, WorkerThreadProc, nullptr, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (g_hWorkerThread)
        {
            PostThreadMessageW(GetThreadId(g_hWorkerThread), WM_QUIT, 0, 0);
            CloseHandle(g_hWorkerThread);
            g_hWorkerThread = nullptr;
        }
    }

    return TRUE;
}