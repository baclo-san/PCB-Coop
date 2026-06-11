/*
 * th07_coop.dll — PCB (Touhou 7) co-op: SECOND PLAYER ENTITY.
 *
 * P2 exists as a full clone of P1's player object, updated + drawn every frame
 * by "piggyback" (we detour the player UPDATE FUN_00441fb0 and DRAW FUN_004420b0,
 * both __fastcall(ECX=player_base), and after they run for P1 we call the
 * trampoline AGAIN with ECX = our P2 base — no task registration needed).
 * P2 has independent local input (input-swap around its update), and its shots
 * damage enemies (TransferP2Shots moves them into P1's authoritative array).
 *
 * THIS STEP — P2 is KILLABLE (collision):
 *   PCB's player-death collisions are leaf primitives, all called __thiscall
 *   with ECX = the P1 static base 0x4bdad8 (confirmed by disasm: every call
 *   site does `mov ecx, 0x4bdad8`), and all are param-relative — on a hit they
 *   invoke FUN_0043b5c0 + FUN_0043edc0 (param-relative) which set the player's
 *   own state to 2 (dying) and spawn the pichuun at the player's own position.
 *     FUN_0043e260  bullets + enemy-body contact
 *     FUN_0043e6b0  lasers
 *   So we DETOUR these two: after each runs for P1, we call the trampoline once
 *   more with ECX = P2. P2's clone then dies through ZUN's own FSM (the state-2
 *   deathbomb window, life loss, respawn) which already runs via P2's piggyback
 *   update. No re-implementation of the hit math or pool iteration.
 *   Resources (lives/cherry) are still SHARED at this stage — P2's death FSM
 *   drives the same global counters P1's does (separate resources come next).
 *   Graze (FUN_0043e3b0) and item collection are resource concerns, deferred.
 *   NOTE: FUN_0043e4e0 is the player-area OVERLAP test (param-relative bool),
 *   NOT the collect/credit logic — that lives in the item-update loop that calls
 *   it. Per-player cherry attribution must hook that caller; see
 *   docs/th07_cherry_determinism.md §6.
 *
 * SEPARATE RESOURCES (this step): P2 has its own BOMBS + POWER, field-swapped
 *   into the shared resource struct (*0x626278) around P2's update (write P2's
 *   bombs/power into +0x68/+0x7c, run update, capture, restore P1's). Score/graze
 *   stay team-shared (only change outside P2's update). LIVES stay SHARED for now:
 *   routing P2's death through ZUN's death-commit while the shared life count
 *   doesn't drop crashes its global death-accounting. Separate lives will come
 *   with ghost mode, which handles P2's death directly instead of via the commit.
 *   P2's bombs/power are logged to coop_log.txt on change until the P2 HUD exists.
 *
 * BOSS / ENEMY HP SCALING (Tier-1): two players ~= 2x DPS, so spell/life phases
 *   end twice as fast. We detour the per-enemy ECL interpreter (FUN_00424290)
 *   and multiply the per-phase max-life cap (+0xd30) by the active player count
 *   the frame the "set life" opcode (re)writes it. PCB's HP model is inverted:
 *   damage climbs in the accumulator (+0xd18) toward the cap, phase ends when
 *   acc >= cap; doubling the cap restores the solo fight length. Auto-arms when
 *   P2 is live (factor = 1 + P2-present); F5 toggles off. Scales all enemies
 *   (TTK-neutral for popcorn, correct for bosses). See docs/th07_boss_hp_scaling.md.
 *
 * Controls (in-stage):
 *   F9  = spawn P2 (clone of P1, offset +24px X). Only when a char is loaded.
 *   F10 = despawn P2 (stop piggyback + free).
 *   F8  = toggle P2 killability (default ON).
 *   F7  = toggle P2 shot damage (default ON).
 *   F6  = toggle P2 separate resources (default ON).
 *   F5  = toggle boss/enemy HP scaling (default ON).
 *   F11 = revive P2 out of ghost mode (debug stand-in for the proximity/graze
 *         resurrection trigger; grants a life if needed, drops P2 into the
 *         respawn-invuln state next to P1).
 *
 * !!! ALL ADDRESSES are build-specific to th07.exe ver 1.00b
 * !!! SHA256 35467EAF8DC7FC85F024F16FB2037255F151CEFDA33CF4867BC9122AAA2E80CA
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include "MinHook.h"

/* ---- build-specific addresses (Ghidra db: th07.exe.c ver 1.00b) ---- */
#define ADDR_PLAYER_BASE    0x004bdad8u            /* static player object      */
#define PLAYER_SIZE         0x000b7e78u            /* sizeof player object       */
#define ADDR_PLAYER_UPDATE  ((LPVOID)0x00441fb0)   /* __fastcall(ecx=player)     */
#define ADDR_PLAYER_DRAW    ((LPVOID)0x004420b0)   /* __fastcall(ecx=player)     */
#define ADDR_INPUT_GAMEPLAY ((volatile uint16_t*)0x004b9e50) /* g_InputGameplay  */

/* gameplay input bits (low 9; same layout the player update reads) */
#define IN_SHOOT 0x01
#define IN_BOMB  0x02
#define IN_FOCUS 0x04
#define IN_UP    0x10
#define IN_DOWN  0x20
#define IN_LEFT  0x40
#define IN_RIGHT 0x80

#define OFF_POS_X     0x930        /* float player X                            */
#define OFF_POS_Y     0x934        /* float player Y                            */
#define OFF_CHARDATA  0xb7e70      /* char speed-table ptr; !=0 ⇒ char loaded   */
#define OFF_STATE     0x2408       /* player state byte: 0 play,1 enter,2 dying, */
                                   /*   3 respawn-invuln, 4 border               */
#define OFF_BOMBING   0x16a20      /* int: nonzero while a bomb is active        */

/* Player-death collision primitives. __thiscall(ECX=player, ...); all callers
 * use `mov ecx,0x4bdad8`. Param-relative: a hit drives the player whose base is
 * in ECX through ZUN's death FSM. We detour + re-invoke each with ECX=P2. */
#define ADDR_COLLIDE_BULLET ((LPVOID)0x0043e260) /* bullets + enemy-body contact */
#define ADDR_COLLIDE_LASER  ((LPVOID)0x0043e6b0) /* lasers                       */

/* Tier-1 boss/enemy HP scaling. The per-phase max-life cap is (re)written once
 * per phase by the "set life" opcode inside the per-enemy ECL command
 * interpreter; we detour it and multiply the cap by the player count on the
 * frame it changes. Cap is stored via a float write but read as int everywhere
 * that matters (death check, bar math) — treat it as int. */
#define ADDR_ECL_INTERP ((LPVOID)0x00424290) /* __fastcall(enemy) ECL cmd interp */
#define OFF_HP_MAX  0xd30   /* int: max life of the current phase (scale target) */
#define OFF_HP_ACC  0xd18   /* int: accumulated damage; ==0 right after set-life */

/* __thiscall modelled as __fastcall: ECX=this, EDX=unused, rest on stack.
 * Stack-arg count + order match, so callee stack cleanup (ret N) matches too. */
typedef int (__fastcall *CollideBulletFn_t)(void *self, void *edx,
                                            float *pos, float *size);
typedef int (__fastcall *CollideLaserFn_t )(void *self, void *edx,
                                            void *a, void *b, void *c,
                                            void *d, int e);

/* ---- separate resources (lives/bombs/power) ----
 * They live in the struct at *(player+8) == DAT_00626278, stored as floats, and
 * are touched BOTH param-relative (accessors FUN_0042d5cd/612, FUN_004325e0) AND
 * absolutely (HUD, item pickup, death power-reset, game-over). So we give P2 its
 * own counters by FIELD-SWAP: write P2's saved lives/bombs/power into the shared
 * struct around P2's update, capture any change, restore P1's. Both access styles
 * then resolve to P2's values during P2's update; everything else stays P1's.
 * Score + graze stay shared (always written via the absolute global). */
/* Resource struct base = the dword stored AT 0x00626278 (i.e. the value of the
 * Ghidra symbol DAT_00626278). The accessors reach it via the score-manager
 * singleton at 0x626270: ECX=0x626270, then *(ECX+8) == *(0x626278). It is NOT
 * reachable from the player base — *(player+8) is unrelated (reading it crashed). */
#define ADDR_RES_PTR  ((volatile uint32_t *)0x00626278)
#define RES_STRUCT_SIZE 200        /* operator_new(200) @0x438359               */
#define RES_LIVES     0x5c         /* float: lives                              */
#define RES_BOMBS     0x68         /* float: bombs                              */
#define RES_POWER     0x7c         /* float: power (0..128)                     */
#define RES_CHECKSUM  0xb0         /* int: ZUN anti-tamper checksum             */
/* set to 1 when a player dies with 0 lives (team game-over / stop-draw flag) */
#define ADDR_GAMEOVER ((volatile unsigned char *)0x0062f64d)

/* ZUN anti-tamper: the resource accessors (bombs/power/lives/score) each guard
 * with FUN_00404fe0, which validates a checksum (res+0xb0) + canaries the HUD
 * refresh FUN_004012b0 keeps consistent. A bomb runs TWO accessors that recompute
 * that checksum FOR P2's swapped values; once we restore P1's values the checksum
 * no longer matches -> the next accessor flags "tampered" and fills a replay/cap
 * array with 0xffffffff, which crashes. So after restoring P1's values we re-run
 * FUN_004012b0 to re-establish the invariant for the restored state. Called with
 * ECX = the score-manager singleton (0x626270); it derefs *(ecx+8) == the struct. */
#define ADDR_HUD_REFRESH     ((HudRefreshFn_t)0x004012b0)
#define ADDR_SCORE_SINGLETON ((uint32_t)0x00626270)
typedef void (__fastcall *HudRefreshFn_t)(uint32_t singleton);

/* player SHOT array (in the player struct). Shots are created/moved param-
 * relative, so P2 makes its own shots — but the shot→enemy damage path only
 * services P1's array. We transfer P2's freshly-fired shots into P1's array so
 * the game's existing collision+draw handle them. Slot pointers reference only
 * shared globals (sprite templates in DAT_004b9e44, shot-type templates), so a
 * raw slot copy is safe. */
#define OFF_SHOTS        0x2444    /* first shot slot                           */
#define SHOT_STRIDE      0x364     /* bytes per slot                            */
#define SHOT_COUNT       0x60      /* 96 slots                                  */
#define OFF_SHOT_ACTIVE  0x34a     /* u16 in slot; !=0 = active                 */

#define P2_SPAWN_OFFSET_X  24.0f

typedef int (__fastcall *PlayerFn_t)(void *self);
typedef int (__fastcall *EclInterpFn_t)(void *self);

static PlayerFn_t s_origUpdate = NULL;
static PlayerFn_t s_origDraw   = NULL;
static CollideBulletFn_t s_origCollideBullet = NULL;
static CollideLaserFn_t  s_origCollideLaser  = NULL;
static EclInterpFn_t     s_origEclInterp     = NULL;

static volatile void *s_p2   = NULL;   /* P2 object base, NULL until spawned     */
static int   s_p2Killable = 1;         /* on by default; F8 toggles              */
static unsigned char s_p2PrevState = 0;/* for death-transition logging           */
static int   s_prevF9 = 0, s_prevF10 = 0, s_prevF8 = 0, s_prevF7 = 0, s_prevF6 = 0, s_prevF11 = 0, s_prevF5 = 0;

/* P2's own LIVES + BOMBS + POWER, field-swapped into the shared struct around P2's
 * update; ZUN's anti-tamper checksum is re-healed afterward (see the heal below).
 * Lives separation is safe now: the life-loss decrement (FUN_0042d5cd) recomputes
 * the checksum, which the heal covers, and P2 running OUT of lives is intercepted
 * into ghost mode instead of triggering team game-over. */
static int   s_p2SepRes = 1;           /* separate lives/bombs/power on; F6 toggles*/
static float s_p2Lives = 0.f, s_p2Bombs = 0.f, s_p2Power = 0.f;
static int   s_p2PrevLives = -1, s_p2PrevBombs = -1, s_p2PrevPower = -1;

/* GHOST MODE: when P2's lives reach 0 its death-commit would set the team
 * game-over flag (DAT_0062f64d). We cancel that, and instead leave P2 as a
 * wandering, invulnerable, non-shooting "ghost" until revived (revive-by-graze
 * + 1up drop come next). Collision is skipped for a ghost so it can't re-die. */
static int   s_p2Ghost = 0;

/* Tier-1: scale enemy/boss HP cap by the active player count. On by default;
 * only takes effect while P2 is live (factor = 1 + (s_p2 != NULL)). F5 toggles. */
static int   s_bossHpScale = 1;
static int   s_bossScaleLogged = 0;

static FILE *s_log = NULL;
static char  s_dir[MAX_PATH];

static int   s_readyFrames = 0;        /* consecutive P1-update frames seen        */
static int   s_autoSpawned = 0;        /* one-shot auto-spawn latch               */
#define AUTO_SPAWN_AFTER 180           /* ~3s of active gameplay, then spawn P2    */

static void Log(const char *fmt, ...)
{
    if (!s_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(s_log, fmt, ap);
    va_end(ap);
    fputc('\n', s_log);
    fflush(s_log);
}

static int P1Ready(void)
{
    /* a character must be loaded (char-data ptr set) for the clone to be valid */
    return *(uint32_t *)(ADDR_PLAYER_BASE + OFF_CHARDATA) != 0;
}

/* P2 local controls (one keyboard, 2 players). Keys chosen to avoid P1's
 * arrows/numpad/Z/X/Shift/Ctrl. This is a stand-in for the netcode-supplied
 * P2 word; the swap mechanism below is exactly what the netcode will drive. */
static int Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static uint16_t ReadP2InputLocal(void)
{
    uint16_t w = 0;
    if (Down('I'))      w |= IN_UP;
    if (Down('K'))      w |= IN_DOWN;
    if (Down('J'))      w |= IN_LEFT;
    if (Down('L'))      w |= IN_RIGHT;
    if (Down(VK_SPACE)) w |= IN_SHOOT;
    if (Down('U'))      w |= IN_FOCUS;
    if (Down('O'))      w |= IN_BOMB;
    return w;
}
static int s_p2InputLogged = 0;

/* Move P2's active shots into free slots of P1's authoritative array, then clear
 * them from P2 (ownership transfer). Runs at the END of P2's update each frame,
 * so P2's array is empty by the time the draw chain runs (no double-draw) and
 * P1's machinery moves/draws/collides them from next frame on. */
static int s_shotXfer = 1;            /* on by default; F7 toggles              */
static int s_shotXferLogged = 0;
static void TransferP2Shots(void *p2)
{
    char *p1arr = (char *)ADDR_PLAYER_BASE + OFF_SHOTS;
    char *p2arr = (char *)p2 + OFF_SHOTS;
    int p1free = 0, moved = 0, i;
    for (i = 0; i < SHOT_COUNT; i++) {
        char *src = p2arr + i * SHOT_STRIDE;
        if (*(short *)(src + OFF_SHOT_ACTIVE) == 0) continue;
        while (p1free < SHOT_COUNT &&
               *(short *)(p1arr + p1free * SHOT_STRIDE + OFF_SHOT_ACTIVE) != 0)
            p1free++;
        if (p1free >= SHOT_COUNT) break;            /* P1 array full — drop rest */
        memcpy(p1arr + p1free * SHOT_STRIDE, src, SHOT_STRIDE);
        *(short *)(src + OFF_SHOT_ACTIVE) = 0;       /* remove from P2 array      */
        p1free++; moved++;
    }
    if (moved && !s_shotXferLogged) {
        Log("shot xfer OK: moved %d P2 shots -> P1 array", moved);
        s_shotXferLogged = 1;
    }
}

static void SpawnP2(void)
{
    if (s_p2) return;                       /* already spawned */
    if (!P1Ready()) { Log("spawn: P1 not ready (no char loaded) — ignored"); return; }

    void *p2 = VirtualAlloc(NULL, PLAYER_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p2) { Log("spawn: VirtualAlloc failed (%lu)", GetLastError()); return; }

    Log("spawn: cloning P1 @0x%08x -> P2 @%p (%u bytes)",
        ADDR_PLAYER_BASE, p2, PLAYER_SIZE);
    memcpy(p2, (const void *)ADDR_PLAYER_BASE, PLAYER_SIZE);

    /* nudge P2 X so it's a visibly distinct sprite */
    *(float *)((char *)p2 + OFF_POS_X) += P2_SPAWN_OFFSET_X;

    s_p2PrevState = *(unsigned char *)((char *)p2 + OFF_STATE);

    /* P2 gets its own LIVES + BOMBS + POWER (field-swapped around its update),
     * seeded from P1's current counts. Running out of lives -> ghost mode. */
    {
        uint32_t res = *ADDR_RES_PTR;
        if (res) {
            s_p2Lives = *(float *)(res + RES_LIVES);
            s_p2Bombs = *(float *)(res + RES_BOMBS);
            s_p2Power = *(float *)(res + RES_POWER);
        }
        s_p2Ghost = 0;
        s_p2PrevLives = s_p2PrevBombs = s_p2PrevPower = -1;
        Log("spawn: P2 lives=%.0f bombs=%.0f power=%.0f (res=0x%08x separate=%d)",
            s_p2Lives, s_p2Bombs, s_p2Power, res, s_p2SepRes);
    }

    s_p2 = p2;                              /* publish last — enables piggyback */
    Log("spawn: P2 live. state=%d  pos=(%.1f,%.1f)",
        *(unsigned char *)((char *)p2 + OFF_STATE),
        *(float *)((char *)p2 + OFF_POS_X),
        *(float *)((char *)p2 + OFF_POS_Y));
}

static void DespawnP2(void)
{
    void *p2 = (void *)s_p2;
    if (!p2) return;
    s_p2 = NULL;                            /* stop piggyback FIRST */
    VirtualFree(p2, 0, MEM_RELEASE);
    Log("despawn: P2 freed");
}

static void ReviveP2(void)
{
    void *p2 = (void *)s_p2;
    if (!p2 || !s_p2Ghost) return;

    /* exit ghost mode and grant a life if needed */
    s_p2Ghost = 0;
    if ((int)s_p2Lives <= 0) s_p2Lives = 1.0f;

    /* set P2 into respawn-invulnerable state and place near P1 */
    *(unsigned char *)((char *)p2 + OFF_STATE) = 3; /* respawn-invuln */
    *(float *)((char *)p2 + OFF_POS_X) = *(float *)((char *)ADDR_PLAYER_BASE + OFF_POS_X) + P2_SPAWN_OFFSET_X;
    *(float *)((char *)p2 + OFF_POS_Y) = *(float *)((char *)ADDR_PLAYER_BASE + OFF_POS_Y);

    Log("P2 revived from GHOST: lives=%.0f state=3 pos=(%.1f,%.1f)", s_p2Lives,
        *(float *)((char *)p2 + OFF_POS_X), *(float *)((char *)p2 + OFF_POS_Y));
}

static void PollHotkeys(void)
{
    int f9  = (GetAsyncKeyState(VK_F9)  & 0x8000) != 0;
    int f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    int f8  = (GetAsyncKeyState(VK_F8)  & 0x8000) != 0;
    int f7  = (GetAsyncKeyState(VK_F7)  & 0x8000) != 0;
    int f6  = (GetAsyncKeyState(VK_F6)  & 0x8000) != 0;
    int f5  = (GetAsyncKeyState(VK_F5)  & 0x8000) != 0;
    int f11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    if (f9  && !s_prevF9)  { Log("F9 edge detected");  SpawnP2(); }
    if (f10 && !s_prevF10) { Log("F10 edge detected"); DespawnP2(); }
    if (f8  && !s_prevF8)  { s_p2Killable = !s_p2Killable; Log("P2 killable %s", s_p2Killable ? "ON" : "OFF"); }
    if (f7  && !s_prevF7)  { s_shotXfer = !s_shotXfer; Log("shot xfer %s", s_shotXfer ? "ON" : "OFF"); }
    if (f6  && !s_prevF6)  { s_p2SepRes = !s_p2SepRes; Log("P2 separate-resources %s", s_p2SepRes ? "ON" : "OFF"); }
    if (f5  && !s_prevF5)  { s_bossHpScale = !s_bossHpScale; Log("boss/enemy HP scaling %s", s_bossHpScale ? "ON" : "OFF"); }
    if (f11 && !s_prevF11) { Log("F11 edge detected"); ReviveP2(); }
    s_prevF9  = f9;
    s_prevF10 = f10;
    s_prevF8  = f8;
    s_prevF7  = f7;
    s_prevF6  = f6;
    s_prevF5  = f5;
    s_prevF11 = f11;
}

/* ---- collision detours: make P2 killable ----
 * Each fires once per (bullet|laser)-vs-player test that the game runs for P1.
 * We run the original for P1 (unchanged), then — when this is the real P1 call
 * (ECX == P1 base) and P2 is live + killable — run it once more with ECX = P2.
 * The primitive is param-relative, so any hit drives P2's own death FSM. We
 * discard P2's return (bullets don't despawn on hitting a player in PCB, so the
 * caller's bullet bookkeeping stays tied to P1's return only). */
static int __fastcall HookedCollideBullet(void *self, void *edx, float *pos, float *size)
{
    int r = s_origCollideBullet(self, edx, pos, size);   /* P1 (unchanged) */
    void *p2 = (void *)s_p2;
    if (s_p2Killable && p2 && (uint32_t)self == ADDR_PLAYER_BASE && !s_p2Ghost)
        s_origCollideBullet(p2, edx, pos, size);          /* P2 — param-relative */
    return r;
}

static int __fastcall HookedCollideLaser(void *self, void *edx,
                                         void *a, void *b, void *c, void *d, int e)
{
    int r = s_origCollideLaser(self, edx, a, b, c, d, e); /* P1 (unchanged) */
    void *p2 = (void *)s_p2;
    if (s_p2Killable && p2 && (uint32_t)self == ADDR_PLAYER_BASE && !s_p2Ghost)
        s_origCollideLaser(p2, edx, a, b, c, d, e);        /* P2 */
    return r;
}

/* ---- boss/enemy HP scaling detour ----
 * Multiply the per-phase max-life cap by the active player count on the frame
 * the "set life" ECL opcode (re)writes it. Guard: the cap actually changed AND
 * the damage accumulator was just reset to 0 — that pair is unique to the
 * set-life command, so a fresh phase is scaled exactly once and never
 * re-multiplied on subsequent frames (when the interpreter leaves the cap as-is,
 * after == before). Only arms while P2 is live, so solo play is untouched. */
static int __fastcall HookedEclInterp(void *self)
{
    int before = *(int *)((char *)self + OFF_HP_MAX);
    int r = s_origEclInterp(self);
    if (s_bossHpScale) {
        int players = s_p2 ? 2 : 1;               /* active player count */
        int after   = *(int *)((char *)self + OFF_HP_MAX);
        if (players > 1 && after != before && after > 0 &&
            *(int *)((char *)self + OFF_HP_ACC) == 0) {
            *(int *)((char *)self + OFF_HP_MAX) = after * players;
            if (!s_bossScaleLogged) {
                Log("boss/enemy HP scaled x%d (%d -> %d)", players, after, after * players);
                s_bossScaleLogged = 1;
            }
        }
    }
    return r;
}

/* ---- detours ---- */
static int __fastcall HookedUpdate(void *self)
{
    int r = s_origUpdate(self);             /* P1 (or whatever ctx the task holds) */

    /* act only off the real P1 object, never re-entrantly off P2 */
    if ((uint32_t)self == ADDR_PLAYER_BASE) {
        PollHotkeys();

        /* auto-spawn after a few seconds of active gameplay (no keyboard needed) */
        if (!s_autoSpawned && !s_p2) {
            if (P1Ready()) {
                if (++s_readyFrames == AUTO_SPAWN_AFTER) {
                    Log("auto-spawn: %d ready frames reached", AUTO_SPAWN_AFTER);
                    SpawnP2();
                    s_autoSpawned = 1;
                }
            } else {
                s_readyFrames = 0;
            }
        }

        void *p2 = (void *)s_p2;
        if (p2) {
            /* INPUT SWAP: P2's update reads g_InputGameplay; point it at P2's
             * own word for the duration, then restore so P1 is unaffected.
             * This is the precise seam the netcode will feed (P2 = high bits). */
            uint16_t p2in  = ReadP2InputLocal();
            uint16_t saved = *ADDR_INPUT_GAMEPLAY;
            if (p2in && !s_p2InputLogged) { Log("P2 input read OK: 0x%03x (key path works)", p2in); s_p2InputLogged = 1; }
            if (s_p2Ghost) p2in &= ~(IN_SHOOT | IN_BOMB);  /* a ghost only wanders */

            *ADDR_INPUT_GAMEPLAY = p2in;

            /* FIELD-SWAP P2's own LIVES + BOMBS + POWER into the shared struct for
             * its update, then capture changes and restore P1's. The accessors
             * recompute ZUN's anti-tamper checksum for P2's values; we re-heal it
             * for the restored P1 state below (else the next accessor flags tamper
             * -> fills the replay/cap array with 0xffffffff -> crash). */
            float *rb = NULL, *rp = NULL, *rl = NULL; float saveB = 0.f, saveP = 0.f, saveL = 0.f;
            uint32_t ckBefore = 0;
            uint32_t res = *ADDR_RES_PTR;
            unsigned char goBefore = *ADDR_GAMEOVER;       /* attribute game-over to P2 */
            if (s_p2SepRes && res) {
                rl = (float *)(res + RES_LIVES);
                rb = (float *)(res + RES_BOMBS); rp = (float *)(res + RES_POWER);
                saveL = *rl; saveB = *rb; saveP = *rp;
                ckBefore = *(volatile uint32_t *)(res + RES_CHECKSUM);
                *rl = s_p2Lives; *rb = s_p2Bombs; *rp = s_p2Power;
            }

            s_origUpdate(p2);               /* trampoline → no detour re-entry */

            if (s_p2SepRes && rb) {
                s_p2Lives = *rl; s_p2Bombs = *rb; s_p2Power = *rp;  /* capture P2's */
                *rl = saveL; *rb = saveB; *rp = saveP;              /* restore P1's */
                if (*(volatile uint32_t *)(res + RES_CHECKSUM) != ckBefore)
                    ADDR_HUD_REFRESH(ADDR_SCORE_SINGLETON);         /* re-heal checksum */
                if ((int)s_p2Lives != s_p2PrevLives || (int)s_p2Bombs != s_p2PrevBombs ||
                    (int)s_p2Power != s_p2PrevPower) {
                    Log("P2 res: lives=%.0f bombs=%.0f power=%.0f", s_p2Lives, s_p2Bombs, s_p2Power);
                    s_p2PrevLives = (int)s_p2Lives;
                    s_p2PrevBombs = (int)s_p2Bombs; s_p2PrevPower = (int)s_p2Power;
                }
            }

            /* P2 ran out of lives during its update -> ZUN set the team game-over
             * flag. Cancel it (P1 plays on) and drop P2 into ghost mode. */
            if (!s_p2Ghost && !goBefore && *ADDR_GAMEOVER) {
                *ADDR_GAMEOVER = goBefore;          /* undo P2-caused game-over */
                s_p2Ghost = 1;
                Log("P2 out of lives -> GHOST mode (wandering, invulnerable)");
            }

            *ADDR_INPUT_GAMEPLAY = saved;   /* restore P1's input immediately */

            /* log P2 death-FSM transitions (0 play,2 dying,3 respawn,1 enter,4 border) */
            {
                unsigned char st = *(unsigned char *)((char *)p2 + OFF_STATE);
                if (st != s_p2PrevState) {
                    Log("P2 state %d -> %d", s_p2PrevState, st);
                    s_p2PrevState = st;
                }
            }

            if (s_shotXfer && !s_p2Ghost) TransferP2Shots(p2);  /* P2 shots -> P1 array */
        }
    }
    return r;
}

static int __fastcall HookedDraw(void *self)
{
    int r = s_origDraw(self);               /* P1 draw */

    if ((uint32_t)self == ADDR_PLAYER_BASE) {
        void *p2 = (void *)s_p2;
        if (p2)
            s_origDraw(p2);
    }
    return r;
}

static int InstallHooks(void)
{
    if (MH_Initialize() != MH_OK) return 0;
    if (MH_CreateHook(ADDR_PLAYER_UPDATE, (LPVOID)&HookedUpdate, (LPVOID*)&s_origUpdate) != MH_OK) return 0;
    if (MH_CreateHook(ADDR_PLAYER_DRAW,   (LPVOID)&HookedDraw,   (LPVOID*)&s_origDraw)   != MH_OK) return 0;
    if (MH_CreateHook(ADDR_COLLIDE_BULLET, (LPVOID)&HookedCollideBullet, (LPVOID*)&s_origCollideBullet) != MH_OK) return 0;
    if (MH_CreateHook(ADDR_COLLIDE_LASER,  (LPVOID)&HookedCollideLaser,  (LPVOID*)&s_origCollideLaser)  != MH_OK) return 0;
    if (MH_CreateHook(ADDR_ECL_INTERP,     (LPVOID)&HookedEclInterp,     (LPVOID*)&s_origEclInterp)     != MH_OK) return 0;
    if (MH_EnableHook(ADDR_PLAYER_UPDATE)  != MH_OK) return 0;
    if (MH_EnableHook(ADDR_PLAYER_DRAW)    != MH_OK) return 0;
    if (MH_EnableHook(ADDR_COLLIDE_BULLET) != MH_OK) return 0;
    if (MH_EnableHook(ADDR_COLLIDE_LASER)  != MH_OK) return 0;
    if (MH_EnableHook(ADDR_ECL_INTERP)     != MH_OK) return 0;
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinst);
        GetModuleFileNameA((HMODULE)hinst, s_dir, MAX_PATH);
        { char *s = strrchr(s_dir, '\\'); if (s) s[1] = '\0'; }
        { char p[MAX_PATH]; snprintf(p, sizeof(p), "%scoop_log.txt", s_dir); s_log = fopen(p, "w"); }
        Log("th07_coop attached. AUTO-spawn P2 ~3s. P2: IJKL move, Space shot, U focus, O bomb. "
            "F5=boss-HP-scale, F6=sep-resources, F7=shot-damage, F8=killable, F9=spawn, F10=despawn, F11=revive.");
        if (!InstallHooks())
            MessageBoxA(NULL, "th07_coop: hook install failed (wrong build/addresses?)",
                        "th07_coop", MB_ICONERROR);
        else
            Log("hooks installed (update @0x441fb0, draw @0x4420b0, "
                "collide-bullet @0x43e260, collide-laser @0x43e6b0, ecl-interp @0x424290)");
        break;
    case DLL_PROCESS_DETACH:
        if (s_log) { Log("detach"); fclose(s_log); }
        MH_Uninitialize();
        break;
    }
    return TRUE;
}
