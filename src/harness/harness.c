/*
 * th07_harness.dll — PCB (Touhou 7) determinism / desync harness
 *
 * Purpose (handoff §5.2): prove PCB's sim stays RNG-locked across two runs
 * from the same seed + same input stream. The RNG call counter @0x0049fe24
 * is the desync oracle: if two runs' counters match every frame, the sim is
 * deterministic; the first divergent line pinpoints the offending call.
 *
 * Mechanism:
 *   - Detour Input_Poll (0x00430b50). Its return value propagates to
 *     g_InputMenu (0x004b9e4c) and, through the replay-record gate in
 *     FUN_00442cd0, to g_InputGameplay (0x004b9e50) — so feeding the poll
 *     return exercises the exact seam the netcode will later own.
 *   - On the FIRST poll (after WinMain has seeded from timeGetTime), force
 *     g_RngState.seed to a fixed value and counter to 0. Everything after
 *     that point is identical across runs iff the sim is deterministic.
 *   - record mode: pass real input through, append each poll's u16 to
 *     input_log.bin, log (poll#, seed, counter) to sync_record.csv.
 *   - replay mode: return u16s from input_log.bin instead of hardware,
 *     log to sync_replay_<pid>.csv.
 *
 * !!! ALL ADDRESSES ARE BUILD-SPECIFIC to the user's un-hashed th07.exe.
 * !!! Pin SHA256 before trusting these offsets on any other copy.
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "MinHook.h"

/* ---- build-specific addresses (Ghidra db: th07.exe.c, SHA256 UNPINNED) ---- */
#define ADDR_INPUT_POLL   ((LPVOID)0x00430b50)   /* u16 poll(void), kbd->bitmask  */
#define ADDR_RNG_SEED     ((volatile uint16_t*)0x0049fe20)
#define ADDR_RNG_COUNTER  ((volatile uint32_t*)0x0049fe24)

#define MODE_RECORD 0
#define MODE_REPLAY 1

typedef unsigned short (__cdecl *InputPoll_t)(void);

static InputPoll_t  s_origPoll   = NULL;
static int          s_mode       = MODE_RECORD;
static uint16_t     s_fixedSeed  = 0x1234;
static int          s_started    = 0;       /* first-poll latch            */
static uint32_t     s_pollIdx    = 0;       /* poll index = harness frame# */
static FILE        *s_fSync      = NULL;
static FILE        *s_fInput     = NULL;    /* record mode only            */
static uint16_t    *s_script     = NULL;    /* replay mode: whole input log */
static uint32_t     s_scriptLen  = 0;
static int          s_scriptDone = 0;
static char         s_dir[MAX_PATH];        /* directory containing the DLL */

static void PathInDir(char *out, size_t n, const char *file)
{
    snprintf(out, n, "%s\\%s", s_dir, file);
}

static void LoadConfig(HMODULE hSelf)
{
    char ini[MAX_PATH], buf[64];

    GetModuleFileNameA(hSelf, s_dir, MAX_PATH);
    char *slash = strrchr(s_dir, '\\');
    if (slash) *slash = '\0';
    PathInDir(ini, sizeof(ini), "harness.ini");

    GetPrivateProfileStringA("harness", "mode", "record", buf, sizeof(buf), ini);
    s_mode = (_stricmp(buf, "replay") == 0) ? MODE_REPLAY : MODE_RECORD;

    GetPrivateProfileStringA("harness", "seed", "0x1234", buf, sizeof(buf), ini);
    s_fixedSeed = (uint16_t)strtoul(buf, NULL, 0);
}

static void OpenLogs(void)
{
    char path[MAX_PATH];

    if (s_mode == MODE_RECORD) {
        PathInDir(path, sizeof(path), "sync_record.csv");
        s_fSync = fopen(path, "w");
        PathInDir(path, sizeof(path), "input_log.bin");
        s_fInput = fopen(path, "wb");
    } else {
        char name[64];
        snprintf(name, sizeof(name), "sync_replay_%lu.csv",
                 (unsigned long)GetCurrentProcessId());
        PathInDir(path, sizeof(path), name);
        s_fSync = fopen(path, "w");

        PathInDir(path, sizeof(path), "input_log.bin");
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            s_scriptLen = (uint32_t)(sz / 2);
            s_script = (uint16_t*)malloc((size_t)sz);
            if (s_script) fread(s_script, 2, s_scriptLen, f);
            else s_scriptLen = 0;
            fclose(f);
        }
    }
    if (s_fSync) fprintf(s_fSync, "poll,seed,counter\n");
}

static unsigned short __cdecl HookedInputPoll(void)
{
    unsigned short real = s_origPoll();
    unsigned short out  = real;

    if (!s_started) {
        /* WinMain has already seeded from timeGetTime by the time the first
         * poll runs (seeding happens right before DInput_Init). Re-seed to a
         * fixed value; reset counter so logs align from 0. */
        *ADDR_RNG_SEED    = s_fixedSeed;
        *ADDR_RNG_COUNTER = 0;
        s_started = 1;
    }

    if (s_mode == MODE_REPLAY) {
        if (s_pollIdx < s_scriptLen) {
            out = s_script[s_pollIdx];
        } else {
            if (!s_scriptDone && s_fSync) {
                fprintf(s_fSync, "# script exhausted at poll %lu — passthrough\n",
                        (unsigned long)s_pollIdx);
                s_scriptDone = 1;       /* fall back to real input (lets you Esc out) */
            }
        }
    } else if (s_fInput) {
        fwrite(&real, sizeof(real), 1, s_fInput);
    }

    if (s_fSync) {
        fprintf(s_fSync, "%lu,%u,%lu\n",
                (unsigned long)s_pollIdx,
                (unsigned)*ADDR_RNG_SEED,
                (unsigned long)*ADDR_RNG_COUNTER);
        if ((s_pollIdx & 63) == 0) {    /* ~once/sec at 60Hz */
            fflush(s_fSync);
            if (s_fInput) fflush(s_fInput);
        }
    }

    s_pollIdx++;
    return out;
}

static int InstallHooks(void)
{
    if (MH_Initialize() != MH_OK) return 0;
    if (MH_CreateHook(ADDR_INPUT_POLL, (LPVOID)&HookedInputPoll,
                      (LPVOID*)&s_origPoll) != MH_OK) return 0;
    if (MH_EnableHook(ADDR_INPUT_POLL) != MH_OK) return 0;
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinst);
        LoadConfig((HMODULE)hinst);
        OpenLogs();
        if (!InstallHooks())
            MessageBoxA(NULL, "th07_harness: hook install failed "
                              "(wrong exe build / addresses?)",
                        "th07_harness", MB_ICONERROR);
        break;
    case DLL_PROCESS_DETACH:
        if (s_fSync)  { fflush(s_fSync);  fclose(s_fSync);  }
        if (s_fInput) { fflush(s_fInput); fclose(s_fInput); }
        MH_Uninitialize();
        break;
    }
    return TRUE;
}
