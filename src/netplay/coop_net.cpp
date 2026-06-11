/*
 * th07_coop_net.dll — Fork A integration: wire the engine-agnostic netcode core
 * (netcode.cpp / Connection.cpp) into th07.exe by detouring its replay tasks.
 *
 * This is the A2 (gameplay-only lockstep) path documented in
 * docs/th07_fork_a_integration.md. It:
 *   1. reads role/peer/delay/seed from coop_net.ini (stand-in for the host/join
 *      ConnectionUI — a proper UI + seed handshake is the next step),
 *   2. brings up the UDP transport (host or guest) and marks the link connected,
 *   3. detours FUN_00442cd0 (ReplayRecord) to overwrite g_InputGameplay with the
 *      lockstep-merged word each logic frame,
 *   4. detours FUN_00442c60 (game-start init) to force the shared RNG seed,
 *   5. resets the netcode when a new game restarts the frame counter.
 *
 * !!! NOT YET TESTED IN-GAME. Compile-verified only (Linux mingw + the netcode
 *     self-tests). Every address below is read from PCBdecomp.c and cited in
 *     docs/th07_fork_a_integration.md. Build-specific to th07.exe ver 1.00b,
 *     SHA256 35467EAF8DC7FC85F024F16FB2037255F151CEFDA33CF4867BC9122AAA2E80CA.
 *
 * Pairs with src/coop/coop.c (Fork B, the P2 entity). Once this feeds P2 input
 * into the high bits of g_InputGameplay, coop.c's P2 reads it instead of the
 * local keyboard. Until both are loaded together, this DLL alone just locksteps
 * P1's input across the two machines (proving the netcode premise live).
 */

#include "netcode.hpp"   // pulls winsock2.h before windows.h (order matters)
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include "MinHook.h"

/* ---- build-specific addresses (PCBdecomp.c, ver 1.00b) ---- */
#define ADDR_REPLAY_RECORD ((LPVOID)0x00442cd0)  /* FUN_00442cd0 __fastcall(int*) */
#define ADDR_GAME_START    ((LPVOID)0x00442c60)  /* FUN_00442c60 __fastcall(int)  */
#define ADDR_INPUT_MENU     ((volatile uint16_t*)0x004b9e4c) /* g_InputMenu        */
#define ADDR_INPUT_GAMEPLAY ((volatile uint16_t*)0x004b9e50) /* g_InputGameplay    */
#define ADDR_RNG_SEED       ((volatile uint16_t*)0x0049fe20) /* g_RngState.seed    */

typedef int (__fastcall *RecordFn_t)(int *self);   /* ECX = record task; self[0]=frame */
typedef int (__fastcall *StartFn_t )(int self);    /* ECX = start task */

static RecordFn_t s_origRecord = NULL;
static StartFn_t  s_origStart  = NULL;

static FILE *s_log = NULL;
static char  s_dir[MAX_PATH];
static int   s_prevFrame = -1;       /* last seen logic frame (reset detector)   */
static int   s_started   = 0;        /* transport up                             */

static void Log(const char *fmt, ...)
{
    if (!s_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(s_log, fmt, ap);
    va_end(ap);
    fputc('\n', s_log);
    fflush(s_log);
}

/* ---- host-environment callbacks the netcode core calls ---- */
static unsigned short ReadLocalInput(void) { return *ADDR_INPUT_MENU; }
static unsigned short ReadRngSeed(void)    { return *ADDR_RNG_SEED;   }

/* ---- config ----
 * coop_net.ini next to the DLL:
 *   [net]
 *   role  = host        ; or guest
 *   peer  = 127.0.0.1   ; guest: host's IP (ignored for host)
 *   port  = 47000       ; host: listen port / guest: host's port
 *   local = 47001       ; guest: local bind port (ignored for host)
 *   delay = 2           ; input delay frames (both sides must match)
 *   seed  = 0x1234      ; shared start RNG seed (host authoritative)
 */
static int   s_isHost   = 1;
static char  s_peer[64] = "127.0.0.1";
static int   s_port     = 47000;
static int   s_localPort= 47001;
static int   s_delay    = 2;
static uint16_t s_seed  = 0x1234;

static void LoadConfig(HMODULE hSelf)
{
    char ini[MAX_PATH], buf[64];
    GetModuleFileNameA(hSelf, s_dir, MAX_PATH);
    char *slash = strrchr(s_dir, '\\');
    if (slash) slash[1] = '\0';
    snprintf(ini, sizeof(ini), "%scoop_net.ini", s_dir);

    GetPrivateProfileStringA("net", "role", "host", buf, sizeof(buf), ini);
    s_isHost = (_stricmp(buf, "guest") != 0);
    GetPrivateProfileStringA("net", "peer", "127.0.0.1", s_peer, sizeof(s_peer), ini);
    s_port      = (int)GetPrivateProfileIntA("net", "port",  47000, ini);
    s_localPort = (int)GetPrivateProfileIntA("net", "local", 47001, ini);
    s_delay     = (int)GetPrivateProfileIntA("net", "delay", 2, ini);
    GetPrivateProfileStringA("net", "seed", "0x1234", buf, sizeof(buf), ini);
    s_seed = (uint16_t)strtoul(buf, NULL, 0);
}

static void StartNet(void)
{
    NetcodeCallbacks cb = { ReadLocalInput, ReadRngSeed };
    Netcode_SetCallbacks(cb);

    bool ok = s_isHost
        ? Netcode_StartHost("", s_port, AF_INET)
        : Netcode_StartGuest(s_peer, s_port, s_localPort, AF_INET);
    if (!ok) { Log("transport start FAILED (role=%s)", s_isHost ? "host" : "guest"); return; }

    /* A2 bring-up: both configs carry the same delay+seed, so we declare the
     * link connected immediately. A real handshake (host sends Ctrl_Set_InitSetting
     * with rng_seed_init; guest adopts it) is the next step — see the doc. */
    Netcode_SetConnected(true, s_delay, s_seed);
    s_started = 1;
    Log("netcode up: role=%s peer=%s port=%d local=%d delay=%d seed=0x%04x",
        s_isHost ? "host" : "guest", s_peer, s_port, s_localPort, s_delay, s_seed);
}

/* ---- detour: per-frame input injection (FUN_00442cd0) ----
 * ZUN's original sets g_InputGameplay = g_InputMenu (and g_InputGameplayPrev =
 * old g_InputGameplay). We then overwrite g_InputGameplay with the merged
 * lockstep word. Because last frame we wrote the merged word into g_InputGameplay,
 * the original's "prev = gameplay" already captured last frame's MERGED value, so
 * edge-detection stays correct. */
static int __fastcall HookedRecord(int *self)
{
    int r = s_origRecord(self);              /* g_InputGameplay = g_InputMenu */

    int frame = self[0];                     /* netcode logic-frame index */

    /* new-game detection: frame counter jumped backwards (task re-created). */
    if (frame < s_prevFrame) {
        Netcode_Reset();
        Log("frame reset (%d -> %d) — Netcode_Reset()", s_prevFrame, frame);
    }
    s_prevFrame = frame;

    if (Netcode_IsConnected()) {
        int ctrl = 0;
        unsigned short merged = Netcode_GetInput_Net(frame, /*is_in_UI=*/false, ctrl);
        *ADDR_INPUT_GAMEPLAY = merged;
        /* ctrl (cheats/quit/restart) is resolved but not yet acted on — TODO. */
    }
    return r;
}

/* ---- detour: seed sync (FUN_00442c60) ----
 * Force the shared seed before the original snapshots it + zeroes the counter,
 * mirroring ZUN's own replay seed-restore (PCBdecomp.c:27909). */
static int __fastcall HookedStart(int self)
{
    if (Netcode_IsConnected())
        *ADDR_RNG_SEED = Netcode_GetInitSeed();
    return s_origStart(self);
}

static int InstallHooks(void)
{
    if (MH_Initialize() != MH_OK) return 0;
    if (MH_CreateHook(ADDR_REPLAY_RECORD, (LPVOID)&HookedRecord, (LPVOID*)&s_origRecord) != MH_OK) return 0;
    if (MH_CreateHook(ADDR_GAME_START,    (LPVOID)&HookedStart,  (LPVOID*)&s_origStart)  != MH_OK) return 0;
    if (MH_EnableHook(ADDR_REPLAY_RECORD) != MH_OK) return 0;
    if (MH_EnableHook(ADDR_GAME_START)    != MH_OK) return 0;
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(hinst);
        LoadConfig((HMODULE)hinst);
        char p[MAX_PATH]; snprintf(p, sizeof(p), "%scoop_net_log.txt", s_dir);
        s_log = fopen(p, "w");
        Log("th07_coop_net attached.");
        StartNet();
        if (!InstallHooks())
            MessageBoxA(NULL, "th07_coop_net: hook install failed (wrong build/addresses?)",
                        "th07_coop_net", MB_ICONERROR);
        else
            Log("hooks installed (record @0x442cd0, start @0x442c60)");
        break;
    }
    case DLL_PROCESS_DETACH:
        if (s_log) { Log("detach"); fclose(s_log); }
        MH_Uninitialize();
        break;
    }
    return TRUE;
}
