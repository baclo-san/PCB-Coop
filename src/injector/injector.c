/*
 * injector.exe — launch th07.exe suspended, inject th07_harness.dll
 * before WinMain runs, then resume.
 *
 * Usage: injector.exe <path\to\th07.exe> [path\to\th07_harness.dll]
 *   DLL defaults to th07_harness.dll next to the injector.
 *   Working directory is set to the exe's folder (the game loads its
 *   data files relative to cwd).
 *
 * Must be built 32-bit (th07.exe is x86).
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

static int Fail(const char *what)
{
    fprintf(stderr, "injector: %s failed (GetLastError=%lu)\n",
            what, GetLastError());
    return 1;
}

int main(int argc, char **argv)
{
    char exePath[MAX_PATH], dllPath[MAX_PATH], exeDir[MAX_PATH];

    if (argc < 2) {
        fprintf(stderr, "usage: injector.exe <th07.exe> [harness.dll]\n");
        return 1;
    }
    if (!GetFullPathNameA(argv[1], MAX_PATH, exePath, NULL))
        return Fail("GetFullPathName(exe)");

    if (argc >= 3) {
        if (!GetFullPathNameA(argv[2], MAX_PATH, dllPath, NULL))
            return Fail("GetFullPathName(dll)");
    } else {
        /* default: th07_harness.dll next to this injector */
        GetModuleFileNameA(NULL, dllPath, MAX_PATH);
        char *s = strrchr(dllPath, '\\');
        if (s) strcpy(s + 1, "th07_harness.dll");
    }
    if (GetFileAttributesA(dllPath) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "injector: DLL not found: %s\n", dllPath);
        return 1;
    }

    strcpy(exeDir, exePath);
    {
        char *s = strrchr(exeDir, '\\');
        if (s) *s = '\0';
    }

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(exePath, NULL, NULL, NULL, FALSE,
                        CREATE_SUSPENDED, NULL, exeDir, &si, &pi))
        return Fail("CreateProcess");

    /* write DLL path into target, LoadLibraryA it via remote thread */
    SIZE_T len = strlen(dllPath) + 1;
    LPVOID remoteMem = VirtualAllocEx(pi.hProcess, NULL, len,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        TerminateProcess(pi.hProcess, 1);
        return Fail("VirtualAllocEx");
    }
    if (!WriteProcessMemory(pi.hProcess, remoteMem, dllPath, len, NULL)) {
        TerminateProcess(pi.hProcess, 1);
        return Fail("WriteProcessMemory");
    }

    /* LoadLibraryA lives in kernel32, which is mapped at the same base in every
     * process on a given boot — so the address resolved here is valid in the
     * target. (32-bit injector → 32-bit target: required.) */
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    LPTHREAD_START_ROUTINE loadLib =
        (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryA");
    if (!loadLib) {
        TerminateProcess(pi.hProcess, 1);
        return Fail("GetProcAddress(LoadLibraryA)");
    }

    HANDLE hThread = CreateRemoteThread(pi.hProcess, NULL, 0, loadLib,
                                        remoteMem, 0, NULL);
    if (!hThread) {
        TerminateProcess(pi.hProcess, 1);
        return Fail("CreateRemoteThread");
    }

    /* wait for LoadLibraryA to return; its exit code is the loaded HMODULE
     * (low 32 bits) — 0 means the DLL failed to load. */
    WaitForSingleObject(hThread, INFINITE);
    DWORD loadRet = 0;
    GetExitCodeThread(hThread, &loadRet);
    CloseHandle(hThread);
    VirtualFreeEx(pi.hProcess, remoteMem, 0, MEM_RELEASE);

    if (loadRet == 0) {
        fprintf(stderr, "injector: LoadLibraryA returned NULL in target "
                        "(DLL failed to load: %s)\n", dllPath);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 1;
    }

    /* DLL is in and its DllMain (hooks installed) has run before WinMain. Go. */
    ResumeThread(pi.hThread);

    printf("injector: launched %s\n  dll=%s\n  injected OK (hmodule low32=0x%08lx)\n",
           exePath, dllPath, loadRet);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}