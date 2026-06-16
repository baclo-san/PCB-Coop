/*
 * launcher.exe — the front door for the PCB co-op mod.
 *
 * A small Win32 dialog (modelled on the EoSD co-op mod's ConnectionUI): the
 * player types an IP / port / input-delay, then clicks one of three buttons:
 *
 *     [ Host Game ]   — be P1, listen for a guest
 *     [ Connect ]     — be P2, dial the host's IP
 *     [ Local Co-op ] — single PC, P2 on the IJKL keyboard (no network)
 *
 * Whichever they pick, the launcher writes the matching coop.ini next to the
 * DLL, then injects th07_coop.dll into th07.exe and launches it. The DLL's
 * built-in handshake does the actual connecting at the title screen — so the
 * launcher is a config-and-launch front-end, not a live socket. That keeps the
 * one UDP socket entirely inside the game process where it belongs.
 *
 * Ship it next to th07.exe + th07_coop.dll (the "beta" drop). It auto-finds
 * th07.exe in its own folder; Browse… overrides.
 *
 * Build 32-bit (it injects a 32-bit DLL into a 32-bit game).
 */

#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define APP_TITLE  "PCB Co-op Launcher (beta)"
#define DLL_NAME   "th07_coop.dll"
#define EXE_NAME   "th07.exe"

/* control IDs */
#define IDC_IP        1001
#define IDC_PORT      1002
#define IDC_LOCAL     1003
#define IDC_DELAY     1004
#define IDC_FADE      1005
#define IDC_EXE       1006
#define IDC_BROWSE    1007
#define IDC_HOST      1008
#define IDC_CONNECT   1009
#define IDC_LOCALCOOP 1010
#define IDC_STATUS    1011
#define IDC_SUPPRESS  1012

static HWND g_ip, g_port, g_local, g_delay, g_fade, g_suppress, g_exe, g_status;
static char g_dir[MAX_PATH];      /* launcher's own folder (trailing '\\')   */
static char g_iniPath[MAX_PATH];  /* g_dir + coop.ini                        */
static char g_dllPath[MAX_PATH];  /* g_dir + th07_coop.dll                   */

/* ---- small helpers -------------------------------------------------------- */

static void Status(const char *s) { SetWindowTextA(g_status, s); }

static int GetEditInt(HWND h, int fallback)
{
    char b[64] = {0};
    GetWindowTextA(h, b, sizeof(b));
    if (!b[0]) return fallback;
    return atoi(b);
}

static void GetEditStr(HWND h, char *out, int cap)
{
    out[0] = 0;
    GetWindowTextA(h, out, cap);
}

/* random nonzero 16-bit seed — the host pushes it to the guest, so each game
 * gets a fresh RNG (otherwise every run plays an identical pattern). */
static unsigned RandSeed(void)
{
    unsigned s = (unsigned)GetTickCount() ^ ((unsigned)GetCurrentProcessId() << 8);
    s = s * 1103515245u + 12345u;
    s = (s >> 8) & 0xFFFFu;
    if (s == 0) s = 0x1234;
    return s;
}

/* ---- coop.ini writer ------------------------------------------------------ */
/* role: 0 = local (no net), 1 = host, 2 = guest. Writes the whole file so the
 * three modes never leave stale keys from a previous launch. */
static void WriteIni(int role)
{
    char ip[128], buf[64];
    int port  = GetEditInt(g_port,  47000);
    int local = GetEditInt(g_local, 47001);
    int delay = GetEditInt(g_delay, 2);
    int fade  = (SendMessageA(g_fade, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    int sup   = (SendMessageA(g_suppress, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    GetEditStr(g_ip, ip, sizeof(ip));
    if (!ip[0]) strcpy(ip, "127.0.0.1");
    if (delay < 0)  delay = 0;
    if (delay > 10) delay = 10;

    WritePrivateProfileStringA("coop", "proximity_fade", fade ? "1" : "0", g_iniPath);
    /* suppress_p2: the netplay desync isolation checkbox — write its state every
     * launch (like fade) so what the box shows is exactly what the DLL reads. */
    WritePrivateProfileStringA("coop", "suppress_p2", sup ? "1" : "0", g_iniPath);
    /* cherry_both_full (B2): no checkbox yet — surface it (default 0) only when
     * absent, so a hand-set value is never clobbered. */
    if (GetPrivateProfileIntA("coop", "cherry_both_full", -1, g_iniPath) < 0)
        WritePrivateProfileStringA("coop", "cherry_both_full", "0", g_iniPath);

    WritePrivateProfileStringA("net", "enabled", role ? "1" : "0", g_iniPath);
    WritePrivateProfileStringA("net", "role", role == 2 ? "guest" : "host", g_iniPath);
    WritePrivateProfileStringA("net", "peer", ip, g_iniPath);
    sprintf(buf, "%d", port);  WritePrivateProfileStringA("net", "port",  buf, g_iniPath);
    sprintf(buf, "%d", local); WritePrivateProfileStringA("net", "local", buf, g_iniPath);
    sprintf(buf, "%d", delay); WritePrivateProfileStringA("net", "delay", buf, g_iniPath);

    /* host mints a fresh seed each run and pushes it to the guest; guest's value
     * is ignored on the wire, so just leave whatever's there for guest/local. */
    if (role == 1) {
        sprintf(buf, "0x%04X", RandSeed());
        WritePrivateProfileStringA("net", "seed", buf, g_iniPath);
    }
}

/* ---- injector (same recipe as injector.c: suspended launch + LoadLibrary) -- */
static int LaunchInjected(const char *exePath, char *errOut, int errCap)
{
    char exeDir[MAX_PATH];
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    LPVOID remoteMem;
    SIZE_T len;
    HMODULE k32;
    LPTHREAD_START_ROUTINE loadLib;
    HANDLE hThread;
    DWORD loadRet = 0;

    if (GetFileAttributesA(g_dllPath) == INVALID_FILE_ATTRIBUTES) {
        snprintf(errOut, errCap, "%s not found next to the launcher.", DLL_NAME);
        return 0;
    }
    if (GetFileAttributesA(exePath) == INVALID_FILE_ATTRIBUTES) {
        snprintf(errOut, errCap, "%s not found:\n%s", EXE_NAME, exePath);
        return 0;
    }

    strcpy(exeDir, exePath);
    { char *s = strrchr(exeDir, '\\'); if (s) *s = '\0'; }

    if (!CreateProcessA(exePath, NULL, NULL, NULL, FALSE,
                        CREATE_SUSPENDED, NULL, exeDir, &si, &pi)) {
        snprintf(errOut, errCap, "CreateProcess failed (err=%lu).", GetLastError());
        return 0;
    }

    len = strlen(g_dllPath) + 1;
    remoteMem = VirtualAllocEx(pi.hProcess, NULL, len,
                               MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        TerminateProcess(pi.hProcess, 1);
        snprintf(errOut, errCap, "VirtualAllocEx failed (err=%lu).", GetLastError());
        return 0;
    }
    if (!WriteProcessMemory(pi.hProcess, remoteMem, g_dllPath, len, NULL)) {
        TerminateProcess(pi.hProcess, 1);
        snprintf(errOut, errCap, "WriteProcessMemory failed (err=%lu).", GetLastError());
        return 0;
    }

    k32 = GetModuleHandleA("kernel32.dll");
    loadLib = (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryA");
    hThread = CreateRemoteThread(pi.hProcess, NULL, 0, loadLib, remoteMem, 0, NULL);
    if (!hThread) {
        TerminateProcess(pi.hProcess, 1);
        snprintf(errOut, errCap, "CreateRemoteThread failed (err=%lu).", GetLastError());
        return 0;
    }
    WaitForSingleObject(hThread, INFINITE);
    GetExitCodeThread(hThread, &loadRet);
    CloseHandle(hThread);
    VirtualFreeEx(pi.hProcess, remoteMem, 0, MEM_RELEASE);

    if (loadRet == 0) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        snprintf(errOut, errCap, "DLL failed to load in the game (LoadLibrary "
                                 "returned NULL).\nWrong th07.exe version?");
        return 0;
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

/* role: 0 local, 1 host, 2 guest */
static void DoLaunch(HWND hWnd, int role)
{
    char exePath[MAX_PATH], err[512];
    GetEditStr(g_exe, exePath, sizeof(exePath));
    if (!exePath[0]) {
        MessageBoxA(hWnd, "Point me at th07.exe first (Browse…).",
                    APP_TITLE, MB_OK | MB_ICONWARNING);
        return;
    }
    if (role == 2) {
        char ip[128];
        GetEditStr(g_ip, ip, sizeof(ip));
        if (!ip[0]) {
            MessageBoxA(hWnd, "Enter the host's IP address to connect.",
                        APP_TITLE, MB_OK | MB_ICONWARNING);
            SetFocus(g_ip);
            return;
        }
    }

    /* remember the th07.exe path for next time (QoL) */
    WritePrivateProfileStringA("launcher", "exe_path", exePath, g_iniPath);

    WriteIni(role);
    Status(role == 0 ? "Launching (local co-op)…"
          : role == 1 ? "Launching as HOST — sit at the title until the guest joins…"
                      : "Launching as GUEST — dialing the host…");

    if (!LaunchInjected(exePath, err, sizeof(err))) {
        MessageBoxA(hWnd, err, APP_TITLE, MB_OK | MB_ICONERROR);
        Status("Launch failed.");
        return;
    }
    /* injected and running — our work is done. */
    PostQuitMessage(0);
}

/* ---- Browse for th07.exe -------------------------------------------------- */
static void OnBrowse(HWND hWnd)
{
    char path[MAX_PATH] = {0};
    OPENFILENAMEA ofn = { sizeof(ofn) };
    ofn.hwndOwner   = hWnd;
    ofn.lpstrFilter = "th07.exe\0th07.exe\0Executables\0*.exe\0All files\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = sizeof(path);
    ofn.lpstrTitle  = "Locate th07.exe (Perfect Cherry Blossom)";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameA(&ofn))
        SetWindowTextA(g_exe, path);
}

/* ---- prefill from a previous coop.ini -------------------------------------- */
static void Prefill(void)
{
    char buf[128], exe[MAX_PATH];
    int fade, port, local, delay;

    GetPrivateProfileStringA("net", "peer", "127.0.0.1", buf, sizeof(buf), g_iniPath);
    SetWindowTextA(g_ip, buf);
    port  = (int)GetPrivateProfileIntA("net", "port",  47000, g_iniPath);
    local = (int)GetPrivateProfileIntA("net", "local", 47001, g_iniPath);
    delay = (int)GetPrivateProfileIntA("net", "delay", 2,     g_iniPath);
    fade  = (int)GetPrivateProfileIntA("coop", "proximity_fade", 1, g_iniPath); /* default ON */
    sprintf(buf, "%d", port);  SetWindowTextA(g_port,  buf);
    sprintf(buf, "%d", local); SetWindowTextA(g_local, buf);
    sprintf(buf, "%d", delay); SetWindowTextA(g_delay, buf);
    SendMessageA(g_fade, BM_SETCHECK, fade ? BST_CHECKED : BST_UNCHECKED, 0);
    {   /* diagnostic: P2-suppress isolation flag (default off) */
        int sup = (int)GetPrivateProfileIntA("coop", "suppress_p2", 0, g_iniPath);
        SendMessageA(g_suppress, BM_SETCHECK, sup ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    /* th07.exe: prefer the path remembered from a previous run, then one sitting
     * next to the launcher (the intended drop). */
    GetPrivateProfileStringA("launcher", "exe_path", "", exe, sizeof(exe), g_iniPath);
    if (exe[0] && GetFileAttributesA(exe) != INVALID_FILE_ATTRIBUTES) {
        SetWindowTextA(g_exe, exe);
    } else {
        snprintf(exe, sizeof(exe), "%s%s", g_dir, EXE_NAME);
        if (GetFileAttributesA(exe) != INVALID_FILE_ATTRIBUTES)
            SetWindowTextA(g_exe, exe);
    }
}

/* ---- window plumbing ------------------------------------------------------ */
static HWND mk(HWND p, const char *cls, const char *txt, DWORD style,
               int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowA(cls, txt, WS_CHILD | WS_VISIBLE | style,
                           x, y, w, h, p, (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return c;
}

static void CreateControls(HWND w)
{
    int lblx = 16, edx = 130, edw = 250, y = 16, row = 34;

    mk(w, "STATIC", "Host IP (guest):", WS_VISIBLE, lblx, y+3, 110, 20, 0);
    g_ip = mk(w, "EDIT", "", WS_BORDER | ES_AUTOHSCROLL, edx, y, edw, 24, IDC_IP);
    y += row;
    mk(w, "STATIC", "Port:", WS_VISIBLE, lblx, y+3, 110, 20, 0);
    g_port = mk(w, "EDIT", "", WS_BORDER | ES_NUMBER, edx, y, 90, 24, IDC_PORT);
    y += row;
    mk(w, "STATIC", "Listen port:", WS_VISIBLE, lblx, y+3, 110, 20, 0);
    g_local = mk(w, "EDIT", "", WS_BORDER | ES_NUMBER, edx, y, 90, 24, IDC_LOCAL);
    y += row;
    mk(w, "STATIC", "Input delay (0-10):", WS_VISIBLE, lblx, y+3, 110, 20, 0);
    g_delay = mk(w, "EDIT", "", WS_BORDER | ES_NUMBER, edx, y, 90, 24, IDC_DELAY);
    y += row;
    g_fade = mk(w, "BUTTON", "Fade the other player when they overlap you",
                BS_AUTOCHECKBOX, edx, y, 300, 22, IDC_FADE);
    y += row;
    g_suppress = mk(w, "BUTTON", "Suppress P2 (netplay desync isolation test)",
                BS_AUTOCHECKBOX, edx, y, 300, 22, IDC_SUPPRESS);
    y += row;

    mk(w, "STATIC", "th07.exe:", WS_VISIBLE, lblx, y+3, 110, 20, 0);
    g_exe = mk(w, "EDIT", "", WS_BORDER | ES_AUTOHSCROLL, edx, y, edw-80, 24, IDC_EXE);
    mk(w, "BUTTON", "Browse…", BS_PUSHBUTTON, edx+edw-72, y, 72, 24, IDC_BROWSE);
    y += row + 8;

    mk(w, "BUTTON", "Host Game",   BS_PUSHBUTTON, 16,  y, 120, 36, IDC_HOST);
    mk(w, "BUTTON", "Connect",     BS_PUSHBUTTON, 145, y, 120, 36, IDC_CONNECT);
    mk(w, "BUTTON", "Local Co-op", BS_PUSHBUTTON, 274, y, 120, 36, IDC_LOCALCOOP);
    y += 48;

    g_status = mk(w, "STATIC",
        "Host & guest: enter the SAME Port. Guest also fills Host IP. "
        "Then Host on one PC, Connect on the other.",
        WS_VISIBLE | SS_LEFTNOWORDWRAP, 16, y, 400, 40, IDC_STATUS);
}

static LRESULT CALLBACK WndProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE:
        CreateControls(w);
        Prefill();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BROWSE:    OnBrowse(w);      return 0;
        case IDC_HOST:      DoLaunch(w, 1);   return 0;
        case IDC_CONNECT:   DoLaunch(w, 2);   return 0;
        case IDC_LOCALCOOP: DoLaunch(w, 0);   return 0;
        }
        break;
    case WM_CLOSE:   DestroyWindow(w);   return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(w, m, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show)
{
    WNDCLASSA wc;
    HWND w;
    MSG msg;
    char title[160];
    (void)hPrev; (void)cmd;

    /* resolve our own folder; coop.ini and the DLL sit beside us */
    GetModuleFileNameA(NULL, g_dir, MAX_PATH);
    { char *s = strrchr(g_dir, '\\'); if (s) s[1] = '\0'; }
    snprintf(g_iniPath, sizeof(g_iniPath), "%scoop.ini", g_dir);
    snprintf(g_dllPath, sizeof(g_dllPath), "%s%s", g_dir, DLL_NAME);

    InitCommonControls();

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = "PcbCoopLauncher";
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassA(&wc);

    sprintf(title, "%s  [net protocol %s]", APP_TITLE, "3.9.5");
    w = CreateWindowA("PcbCoopLauncher", title,
                      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                      CW_USEDEFAULT, CW_USEDEFAULT, 438, 464,
                      NULL, NULL, hInst, NULL);
    if (!w) return 1;
    ShowWindow(w, show);
    UpdateWindow(w);

    while (GetMessageA(&msg, NULL, 0, 0)) {
        if (IsDialogMessageA(w, &msg)) continue;  /* Tab between fields */
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}
