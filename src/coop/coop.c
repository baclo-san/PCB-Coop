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
 *     FUN_0043e3b0  GRAZE  (must also be detoured — see below)
 *   So we DETOUR these: after each runs for P1, we call the trampoline once more
 *   with ECX = P2. P2's clone then dies through ZUN's own FSM (the state-2
 *   deathbomb window, life loss, respawn) which already runs via P2's piggyback
 *   update. No re-implementation of the hit math or pool iteration.
 *   Resources (lives/cherry) are still SHARED at this stage — P2's death FSM
 *   drives the same global counters P1's does (separate resources come next).
 *   !!! GRAZE IS LOAD-BEARING, not just points: for regular bullets the per-bullet
 *   HIT test (FUN_0043e260) is GATED behind the bullet's "grazed" flag (+0xc01) —
 *   a bullet must graze the player before its hit test runs (PCBdecomp.c:14718).
 *   So without detouring graze, P2 grazes nothing -> bullets near P2 never get
 *   flagged -> P2 is never hit-tested against them (only ungated lasers/contact
 *   could hurt it). HookedGraze re-invokes graze for P2 and reports "grazed" if
 *   either player did, setting the flag so P2's bullet hits work + P2 grazes.
 *   Item collection: P2
 *   collects items it overlaps, and items P2 collects are credited to P2's OWN
 *   lives/bombs/power (the collect-time field-swap in HookedCollectOverlap), so
 *   P2's power/bombs grow from its own pickups; team score stays shared. NOTE:
 *   FUN_0043e4e0 is the player-area OVERLAP test (param-relative bool), NOT the
 *   collect/credit logic — that lives in the item-update loop FUN_00432990 that
 *   calls it. CHERRY is not credited by item collection (it fills from shots/
 *   graze) and remains shared for now — see docs/th07_item_collect_credit.md and
 *   docs/th07_cherry_determinism.md §0.
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
 * SHARED TEAM BORDER (user-corrected design): the cherry border is the AUTOMATIC
 *   supernatural border that fires when the shared Cherry+ gauge reaches 50000 — it
 *   costs NO bombs (bombs are a SEPARATE spell-card mechanic). ZUN's border ring is a
 *   SINGLE fixed effect slot (index 404, hardcoded in FUN_0041c610), so we CANNOT run
 *   the border-start FUN_00441960 a second time for P2 — it would steal P1's ring.
 *   Instead P1 keeps the one real, fully-vanilla border (ring + gauge + bonus), and P2
 *   rides along in a RINGLESS "shadow" border: we set P2's state to 4 + the border
 *   flag (0x240d) + copy P1's timer each frame (UpdateTeamBorder, polled before P2's
 *   update), but leave P2's ring ptr null. P2 is then genuinely in state 4 so it is
 *   invincible AND ZUN's own collision (FUN_0043e260/6b0) and bomb handler
 *   (FUN_004409f0) pop the border on a hit / bomb-press exactly like P1 — no death, no
 *   spell card, no bomb spent. We hook only the break leaf FUN_00441bd0
 *   (__thiscall(player,int flag)) to propagate a pop by either player to the other, so
 *   one team border ends for both. Timeout: P1 ends in its own update first, then the
 *   poll retires P2's shadow (score may bank twice — user: acceptable, sync-safe).
 *   P2's bomb is NOT masked: outside the border it casts a normal spell card (spends
 *   P2's own bomb); during the border it pops instead. F4 toggles.
 *   See docs/th07_cherry_determinism.md §0.
 *
 * RESURRECTION (EoSD-mod style, user-specified 2026-06-12): when P2 runs out of
 *   lives it becomes a GHOST — invulnerable, no shot/bomb, and AUTO-WANDERING
 *   (bounces inside the bottom ~1/5 band of the playfield between the side
 *   walls, via synthesized input bits through ZUN's own movement code). The
 *   survivor revives it by GRAZING the ghost (staying within graze range) for
 *   90 consecutive frames: that DONATES one of the survivor's lives (written
 *   into the shared struct + checksum re-heal) — or is FREE when the survivor
 *   has no spare extends. Additionally, dying on the LAST life drops a
 *   guaranteed 1-UP item at the death spot (ZUN's item spawner FUN_004326f0,
 *   type 5), so the survivor can bank a life and then revive. F11 stays as the
 *   debug instant-revive.
 *
 * Controls (in-stage):
 *   F9  = spawn P2 (clone of P1, offset +24px X). Only when a char is loaded.
 *   F10 = despawn P2 (stop piggyback + free).
 *   F8  = toggle P2 killability (default ON).
 *   F7  = toggle P2 shot damage (default ON).
 *   F6  = toggle P2 separate resources (default ON).
 *   F5  = toggle boss/enemy HP scaling (default ON).
 *   F4  = toggle synced cherry team border (default ON).
 *   F11 = debug instant-revive of a ghost P2 (the graze resurrection above is
 *         the real mechanic; this stays as a test shortcut).
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
/* GRAZE test (FUN_0043e3b0, __thiscall(player,pos,size)). CRITICAL: for regular bullets
 * the player-HIT test (FUN_0043e260) is GATED behind the bullet's "grazed" flag (+0xc01)
 * — a bullet must graze the player before its hit test runs (PCBdecomp.c:14718-14731). So
 * P2 only becomes hittable by such bullets if it ALSO grazes them; we re-invoke graze for
 * P2 and report "grazed" if either player did, which sets the bullet flag + credits P2's
 * graze. */
#define ADDR_COLLIDE_GRAZE  ((LPVOID)0x0043e3b0)
/* Player-area OVERLAP test (param-relative bool). The item-update loop calls it
 * per item as the "touched the player -> collect" check, and it is that loop's
 * only caller. We detour it so an item overlapping a live P2 is collected too. */
#define ADDR_COLLECT_OVERLAP ((LPVOID)0x0043e4e0)
/* Item update + collect loop (FUN_00432990, __fastcall(item_mgr)) — the only
 * caller of the overlap test. We bracket it to undo any P2-collected resource
 * swap that the last collected item left in place. */
#define ADDR_ITEM_LOOP ((LPVOID)0x00432990)
/* Cherry-border FSM. The border is the AUTOMATIC supernatural border (fires when the
 * shared Cherry+ gauge hits 50000); it costs no bombs. P1 runs ZUN's real border
 * (vanilla, untouched): FUN_00441960 start (state->4, single ring slot 404),
 * FUN_00441670 timeout-bank, FUN_00441bd0 break. We hook ONLY the break (to propagate a
 * pop between players); P2 rides along in a ringless shadow state we drive directly. */
#define ADDR_BORDER_BREAK ((LPVOID)0x00441bd0) /* __thiscall(player,int flag): hit/bomb pop */
/* border-timer + state fields the FSM writes (player-relative) */
#define OFF_BORDER_T0   0x16a08   /* border countdown (0x21c at start)                 */
#define OFF_BORDER_T1   0x16a04
#define OFF_BORDER_T2   0x16a00   /* first of the 6 border-timer dwords (0x16a00..14)  */
#define OFF_RESPAWN_T   0x23fc    /* post-border/respawn invuln countdown (0x28)       */
#define OFF_BORDER_REQ  0x240d    /* border request/active flag (1 = pressing bomb pops)*/
#define OFF_BORDER_SPR  0xb7e6c   /* border-ring sprite object ptr                     */

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
typedef int (__fastcall *CollectOverlapFn_t)(void *self, void *edx,
                                             float *pos, float *size);
typedef void (__fastcall *ItemLoopFn_t)(void *self);
/* FUN_00441bd0 is __thiscall(player[ECX], int flag[stack]) with `ret 4` (verified vs the
 * binary). Model as __fastcall: ECX=self, EDX unused, flag on the stack. */
typedef void (__fastcall *BorderBreakFn_t)(void *self, void *edx, int flag);

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
static CollideBulletFn_t s_origGraze         = NULL;   /* FUN_0043e3b0, same signature */
static CollideLaserFn_t  s_origCollideLaser  = NULL;
static CollectOverlapFn_t s_origCollectOverlap = NULL;
static EclInterpFn_t     s_origEclInterp     = NULL;
static ItemLoopFn_t      s_origItemLoop       = NULL;

static volatile void *s_p2   = NULL;   /* P2 object base, NULL until spawned     */
static int   s_p2Killable = 1;         /* on by default; F8 toggles              */
static unsigned char s_p2PrevState = 0;/* for death-transition logging           */
static int   s_prevF9 = 0, s_prevF10 = 0, s_prevF8 = 0, s_prevF7 = 0, s_prevF6 = 0, s_prevF11 = 0, s_prevF5 = 0, s_prevF4 = 0;

/* P2's own LIVES + BOMBS + POWER, field-swapped into the shared struct around P2's
 * update; ZUN's anti-tamper checksum is re-healed afterward (see the heal below).
 * Lives separation is safe now: the life-loss decrement (FUN_0042d5cd) recomputes
 * the checksum, which the heal covers, and P2 running OUT of lives is intercepted
 * into ghost mode instead of triggering team game-over. */
static int   s_p2SepRes = 1;           /* separate lives/bombs/power on; F6 toggles*/
static float s_p2Lives = 0.f, s_p2Bombs = 0.f, s_p2Power = 0.f;
static int   s_p2PrevLives = -1, s_p2PrevBombs = -1, s_p2PrevPower = -1;

/* GHOST MODE: when P2's lives reach 0 its death-commit would set the team
 * game-over flag (DAT_0062f64d). We cancel that, and instead leave P2 as an
 * auto-wandering, invulnerable, non-shooting "ghost" until the survivor
 * graze-revives it (EoSD-mod style — see the header). Collision is skipped
 * for a ghost so it can't re-die. */
static int   s_p2Ghost = 0;
static int   s_ghostDirX = 1, s_ghostDirY = 1;  /* auto-wander bounce direction */
static int   s_reviveFrames = 0;       /* consecutive frames P1 grazed the ghost */

/* ZUN's item spawner: __thiscall(item_mgr, float pos[3], int type, int mode) —
 * walks the 1100-slot ring, activates a free slot, sets pos/type/mode
 * (PCBdecomp.c:20244, FUN_004326f0). type 5 = 1up; mode 0 = plain pop-and-fall.
 * The item-manager object is captured in HookedItemLoop. */
typedef int (__fastcall *ItemSpawnFn_t)(void *self, void *edx,
                                        float *pos, int type, int mode);
#define ADDR_ITEM_SPAWN ((ItemSpawnFn_t)0x004326f0)
#define ITEM_TYPE_1UP   5
static volatile void *s_itemMgr = NULL;

/* Tier-1: scale enemy/boss HP cap by the active player count. On by default;
 * only takes effect while P2 is live (factor = 1 + (s_p2 != NULL)). F5 toggles. */
static int   s_bossHpScale = 1;

/* SHARED TEAM BORDER: the automatic Cherry+ supernatural border. P1 keeps the one real
 * ZUN border (single ring slot); P2 rides along in a ringless "shadow" state 4 synced to
 * P1, so both are invincible and either can pop it. On by default; F4 toggles. */
static int   s_p2TeamBorder = 1;
static int   s_p2Shadow     = 0;   /* P2 currently in the ringless shadow border        */
static int   s_borderMirror = 0;   /* re-entrancy guard around break propagation        */
static int   s_borderLogged = 0;
static BorderBreakFn_t s_origBorderBreak = NULL;

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
        s_reviveFrames = 0;
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

/* ---- EoSD-style resurrection (user-specified 2026-06-12) ---- */

/* Ghost auto-movement: bounce inside the bottom ~1/5 band of the playfield
 * (between the side walls, off the band top and the bottom wall). Driven via
 * synthesized input bits through ZUN's own movement code, so it respects the
 * engine's position clamps and the character's speed. Thresholds only steer;
 * the engine clamp is the real wall. Playfield ~384x448 world units. */
#define GHOST_X_MIN   48.0f
#define GHOST_X_MAX  336.0f
#define GHOST_Y_TOP  352.0f            /* top of the bottom-1/5 band (448*4/5) */
#define GHOST_Y_BOT  432.0f

static uint16_t GhostAutoInput(void *p2)
{
    float x = *(float *)((char *)p2 + OFF_POS_X);
    float y = *(float *)((char *)p2 + OFF_POS_Y);
    if (x <= GHOST_X_MIN) s_ghostDirX = 1;  else if (x >= GHOST_X_MAX) s_ghostDirX = -1;
    if (y <= GHOST_Y_TOP) s_ghostDirY = 1;  else if (y >= GHOST_Y_BOT) s_ghostDirY = -1;
    return (uint16_t)((s_ghostDirX > 0 ? IN_RIGHT : IN_LEFT) |
                      (s_ghostDirY > 0 ? IN_DOWN  : IN_UP));
}

/* Guaranteed 1up at the death spot when P2 dies on its last life, so the
 * survivor can bank a life and then graze-revive. Only P1 can collect it
 * (the collect-overlap hook skips a ghost P2). */
static void DropOneUp(void *plr)
{
    void *mgr = (void *)s_itemMgr;
    float pos[3];
    if (!mgr) { Log("1up drop SKIPPED: item manager not seen yet"); return; }
    memcpy(pos, (char *)plr + OFF_POS_X, sizeof(pos));   /* x,y,z are contiguous */
    ADDR_ITEM_SPAWN(mgr, NULL, pos, ITEM_TYPE_1UP, 0);
    Log("P2 last-life death -> dropped 1up at (%.1f,%.1f)", pos[0], pos[1]);
}

/* The survivor grazes the ghost for REVIVE_FRAMES consecutive frames ->
 * donates one life (free when they have no spare extends) and the ghost
 * resurrects next to them. Runs while the shared struct holds P1's values
 * (after the P2 field-swap restore), so the donation is a direct write +
 * checksum re-heal — the same proven pattern as the resource swap. */
#define REVIVE_RADIUS  32.0f
#define REVIVE_FRAMES  90

static void ReviveByGraze(void *p2)
{
    unsigned char p1st = *(unsigned char *)((char *)ADDR_PLAYER_BASE + OFF_STATE);
    float dx = *(float *)((char *)ADDR_PLAYER_BASE + OFF_POS_X)
             - *(float *)((char *)p2 + OFF_POS_X);
    float dy = *(float *)((char *)ADDR_PLAYER_BASE + OFF_POS_Y)
             - *(float *)((char *)p2 + OFF_POS_Y);

    /* P1 must be in a controllable state (play / respawn-invuln / border) */
    if ((p1st == 0 || p1st == 3 || p1st == 4) &&
        dx > -REVIVE_RADIUS && dx < REVIVE_RADIUS &&
        dy > -REVIVE_RADIUS && dy < REVIVE_RADIUS) {
        if (++s_reviveFrames >= REVIVE_FRAMES) {
            uint32_t res = *ADDR_RES_PTR;
            if (res && *(float *)(res + RES_LIVES) >= 1.0f) {
                *(float *)(res + RES_LIVES) -= 1.0f;        /* donate one */
                ADDR_HUD_REFRESH(ADDR_SCORE_SINGLETON);     /* re-heal checksum */
                Log("revive: P1 donated a life (%.0f spare left)",
                    *(float *)(res + RES_LIVES));
            } else {
                Log("revive: P1 has no spare extends -> free revive");
            }
            ReviveP2();
            s_reviveFrames = 0;
        }
    } else {
        s_reviveFrames = 0;
    }
}

static void PollHotkeys(void)
{
    int f9  = (GetAsyncKeyState(VK_F9)  & 0x8000) != 0;
    int f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    int f8  = (GetAsyncKeyState(VK_F8)  & 0x8000) != 0;
    int f7  = (GetAsyncKeyState(VK_F7)  & 0x8000) != 0;
    int f6  = (GetAsyncKeyState(VK_F6)  & 0x8000) != 0;
    int f5  = (GetAsyncKeyState(VK_F5)  & 0x8000) != 0;
    int f4  = (GetAsyncKeyState(VK_F4)  & 0x8000) != 0;
    int f11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    if (f9  && !s_prevF9)  { Log("F9 edge detected");  SpawnP2(); }
    if (f10 && !s_prevF10) { Log("F10 edge detected"); DespawnP2(); }
    if (f8  && !s_prevF8)  { s_p2Killable = !s_p2Killable; Log("P2 killable %s", s_p2Killable ? "ON" : "OFF"); }
    if (f7  && !s_prevF7)  { s_shotXfer = !s_shotXfer; Log("shot xfer %s", s_shotXfer ? "ON" : "OFF"); }
    if (f6  && !s_prevF6)  { s_p2SepRes = !s_p2SepRes; Log("P2 separate-resources %s", s_p2SepRes ? "ON" : "OFF"); }
    if (f5  && !s_prevF5)  { s_bossHpScale = !s_bossHpScale; Log("boss/enemy HP scaling %s", s_bossHpScale ? "ON" : "OFF"); }
    if (f4  && !s_prevF4)  { s_p2TeamBorder = !s_p2TeamBorder; Log("P2 team-border %s", s_p2TeamBorder ? "ON" : "OFF"); }
    if (f11 && !s_prevF11) { Log("F11 edge detected"); ReviveP2(); }
    s_prevF9  = f9;
    s_prevF10 = f10;
    s_prevF8  = f8;
    s_prevF7  = f7;
    s_prevF6  = f6;
    s_prevF5  = f5;
    s_prevF4  = f4;
    s_prevF11 = f11;
}

/* ---- collision detours: make P2 killable ----
 * Each fires once per (bullet|laser)-vs-player test that the game runs for P1.
 * We run the original for P1 (unchanged), then — when this is the real P1 call
 * (ECX == P1 base) and P2 is live + killable — run it once more with ECX = P2.
 * The primitive is param-relative, so any hit drives P2's own death FSM. We
 * discard P2's return (bullets don't despawn on hitting a player in PCB, so the
 * caller's bullet bookkeeping stays tied to P1's return only).
 *
 * Border interaction: with the team border ON and BOTH players in state 4, P2's
 * collision is ALLOWED to run — a hit then breaks P2's border via ZUN's own state-4
 * path (FUN_00441bd0), which our break hook propagates to P1 (a hit pops the team
 * border, vanilla behaviour). We only SKIP P2 when P1 is bordered but P2 is NOT
 * (e.g. P2 respawning), so it isn't unfairly killed during the team border; with the
 * team border OFF we skip P2 for all of P1's border (legacy "both invuln"). */
static int P2CollisionSkipped(void *p2)
{
    int p1border = *(unsigned char *)((char *)ADDR_PLAYER_BASE + OFF_STATE) == 4;
    int p2border = *(unsigned char *)((char *)p2 + OFF_STATE) == 4;
    if (s_p2TeamBorder) return p1border && !p2border;
    return p1border;
}

static int __fastcall HookedCollideBullet(void *self, void *edx, float *pos, float *size)
{
    int r = s_origCollideBullet(self, edx, pos, size);   /* P1 (unchanged) */
    void *p2 = (void *)s_p2;
    if (s_p2Killable && p2 && (uint32_t)self == ADDR_PLAYER_BASE && !s_p2Ghost &&
        !P2CollisionSkipped(p2))
        s_origCollideBullet(p2, edx, pos, size);          /* P2 — param-relative */
    return r;
}

static int __fastcall HookedCollideLaser(void *self, void *edx,
                                         void *a, void *b, void *c, void *d, int e)
{
    int r = s_origCollideLaser(self, edx, a, b, c, d, e); /* P1 (unchanged) */
    void *p2 = (void *)s_p2;
    if (s_p2Killable && p2 && (uint32_t)self == ADDR_PLAYER_BASE && !s_p2Ghost &&
        !P2CollisionSkipped(p2))
        s_origCollideLaser(p2, edx, a, b, c, d, e);        /* P2 */
    return r;
}

/* ---- graze detour: make P2 graze, AND unlock regular-bullet hits on P2 ----
 * Re-invoke the graze test for P2. Returning "grazed" (1) when EITHER player grazes makes
 * the caller set the bullet's grazed flag (+0xc01), which is what gates the per-bullet HIT
 * test (FUN_0043e260) — so without this, regular bullets never get hit-tested against P2.
 * FUN_0043e3b0 returns 1=grazed, 2=cancelled-by-field, 0=nothing; we only upgrade 0->1. */
static int __fastcall HookedGraze(void *self, void *edx, float *pos, float *size)
{
    int r = s_origGraze(self, edx, pos, size);            /* P1 (unchanged) */
    void *p2 = (void *)s_p2;
    if (p2 && (uint32_t)self == ADDR_PLAYER_BASE && !s_p2Ghost && !P2CollisionSkipped(p2)) {
        int r2 = s_origGraze(p2, edx, pos, size);          /* P2 — param-relative */
        if (r == 0 && r2 == 1) r = 1;   /* P2 grazed -> mark bullet grazed so its hit test runs */
    }
    return r;
}

/* ---- item collection: P2 collects items, credited to P2's OWN resources ----
 * The per-item "touched the player -> collect" overlap test (single caller =
 * the item-update loop FUN_00432990). After it runs for P1, if P1 did NOT collect
 * this item but a live, non-ghost P2 overlaps it, we return 1 so the loop collects
 * it. P2's position is the synced merged input, so both netplay machines collect
 * identically (determinism-safe).
 *
 * ATTRIBUTION: the loop credits power/bombs/lives to the shared resource struct in
 * the SAME iteration, right after this test returns. So when P2 is the collector we
 * field-swap P2's lives/bombs/power INTO the struct here (no pre-heal — same as the
 * proven per-update swap) and leave them held; the credit then lands on P2's values.
 * We undo the swap at the START of the next item's overlap test (capturing P2's new
 * totals, restoring P1's, re-healing ZUN's checksum), and once more at the end of
 * the whole loop (for the last collected item). Score/point counters are NOT in the
 * swap set, so team score stays shared. Cherry is not credited by collection at all
 * (see docs/th07_item_collect_credit.md). */
static int   s_p2ResHeld = 0;                 /* P2 res currently swapped in for a collect */
static float s_heldP1Lives, s_heldP1Bombs, s_heldP1Power;
static uint32_t s_heldCk;
static int   s_p2CollectLogged = 0;

static void RestoreHeldRes(void)
{
    if (!s_p2ResHeld) return;
    s_p2ResHeld = 0;
    uint32_t res = *ADDR_RES_PTR;
    if (!res) return;
    s_p2Lives = *(float *)(res + RES_LIVES);          /* capture P2's post-credit totals */
    s_p2Bombs = *(float *)(res + RES_BOMBS);
    s_p2Power = *(float *)(res + RES_POWER);
    *(float *)(res + RES_LIVES) = s_heldP1Lives;      /* restore P1's */
    *(float *)(res + RES_BOMBS) = s_heldP1Bombs;
    *(float *)(res + RES_POWER) = s_heldP1Power;
    if (*(volatile uint32_t *)(res + RES_CHECKSUM) != s_heldCk)
        ADDR_HUD_REFRESH(ADDR_SCORE_SINGLETON);       /* re-heal for the restored P1 state */
}

static int __fastcall HookedCollectOverlap(void *self, void *edx, float *pos, float *size)
{
    RestoreHeldRes();                                  /* undo the previous item's P2 swap */

    int r = s_origCollectOverlap(self, edx, pos, size);   /* P1 (unchanged) */
    void *p2 = (void *)s_p2;
    if ((uint32_t)self == ADDR_PLAYER_BASE && !r && p2 && !s_p2Ghost) {
        r = s_origCollectOverlap(p2, edx, pos, size);      /* P2 — param-relative */
        if (r && s_p2SepRes) {
            /* P2 collected: hold P2's resources in for this item's upcoming credit */
            uint32_t res = *ADDR_RES_PTR;
            if (res) {
                s_heldP1Lives = *(float *)(res + RES_LIVES);
                s_heldP1Bombs = *(float *)(res + RES_BOMBS);
                s_heldP1Power = *(float *)(res + RES_POWER);
                s_heldCk      = *(volatile uint32_t *)(res + RES_CHECKSUM);
                *(float *)(res + RES_LIVES) = s_p2Lives;
                *(float *)(res + RES_BOMBS) = s_p2Bombs;
                *(float *)(res + RES_POWER) = s_p2Power;
                s_p2ResHeld = 1;
                if (!s_p2CollectLogged) { Log("P2 item collect -> P2 resources (first)"); s_p2CollectLogged = 1; }
            }
        }
    }
    return r;
}

/* Bracket the item-update loop so the LAST collected item's P2 swap is undone
 * before anything outside the loop reads the shared resources. */
static void __fastcall HookedItemLoop(void *self)
{
    s_itemMgr = self;                   /* captured for DropOneUp */
    s_origItemLoop(self);
    RestoreHeldRes();
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
        int acc     = *(int *)((char *)self + OFF_HP_ACC);
        /* DIAG (boss-HP game test came back inconclusive): log EVERY cap write —
         * one line per enemy phase init — with the guard verdict, so a single run
         * shows whether a midboss/boss set-life passes through here at all. */
        if (after != before) {
            if (players > 1 && after > 0 && acc == 0) {
                *(int *)((char *)self + OFF_HP_MAX) = after * players;
                Log("eclhp: %08x cap %d -> %d SCALED x%d", (uint32_t)self,
                    before, after * players, players);
            } else {
                Log("eclhp: %08x cap %d -> %d skip (players=%d acc=%d)",
                    (uint32_t)self, before, after, players, acc);
            }
        }
    }
    return r;
}

/* ---- shared team cherry border ----
 * ZUN's border ring is a SINGLE fixed effect slot (index 404, hardcoded in
 * FUN_0041c610 via the border's slot arg=4), so we CANNOT run FUN_00441960 a second
 * time for P2 — it would re-grab slot 404 and steal P1's ring (only one ring can
 * exist). Instead P1 keeps the one real ZUN border (ring + gauge + bonus, fully
 * vanilla) and we put P2 into a RINGLESS "shadow" border: state 4 + the border-active
 * flag (0x240d) + P1's timer copied in, but border-sprite ptr left null. P2 is then
 * genuinely in state 4 — invincible, and ZUN's own collision (FUN_0043e260/6b0) and
 * bomb handler (FUN_004409f0) pop it on a hit / bomb-press exactly like P1 — without
 * owning a ring, so P1's single ring survives and follows P1.
 *
 * Lifecycle is driven by polling P1's state each frame in HookedUpdate (UpdateTeamBorder,
 * run just before P2's update): enter/sync the shadow while P1 is in state 4, retire it
 * when P1 leaves. Timeout needs no special handling — P1 ends in its own update first, the
 * poll then retires P2's shadow before P2's update reaches the bank. Premature pops are
 * propagated by the break hook below. */
static void UpdateTeamBorder(void *p2)
{
    if (!s_p2TeamBorder || s_p2Ghost) return;
    char *p1 = (char *)ADDR_PLAYER_BASE, *pp = (char *)p2;
    int p1border = *(unsigned char *)(p1 + OFF_STATE) == 4;
    int p2state  = *(unsigned char *)(pp + OFF_STATE);
    if (p1border) {
        if (p2state == 0 || (s_p2Shadow && p2state == 4)) {
            /* enter / maintain P2's ringless shadow border, timer synced to P1 */
            memcpy(pp + OFF_BORDER_T2, p1 + OFF_BORDER_T2, 0x18); /* 0x16a00..0x16a14 */
            *(unsigned char *)(pp + OFF_STATE)     = 4;
            *(unsigned char *)(pp + OFF_BORDER_REQ) = 1;          /* so P2's bomb pops it */
            *(int *)(pp + OFF_BORDER_SPR)           = 0;          /* never own a ring */
            if (!s_p2Shadow) {
                s_p2Shadow = 1;
                if (!s_borderLogged) { Log("TEAM cherry border: P2 shares P1's border (shadow)"); s_borderLogged = 1; }
            }
        }
    } else if (s_p2Shadow) {
        if (p2state == 4) {                          /* P1's border ended; retire P2's shadow */
            *(int *)(pp + OFF_BORDER_SPR)           = 0;
            *(unsigned char *)(pp + OFF_STATE)      = 3;          /* post-border invuln */
            *(int *)(pp + OFF_BORDER_T0)            = 0x28;
            *(int *)(pp + OFF_BORDER_T1)            = 0;
            *(int *)(pp + OFF_BORDER_T2)            = 0xfffffc19;
            *(int *)(pp + OFF_RESPAWN_T)            = 0x28;
            *(unsigned char *)(pp + OFF_BORDER_REQ) = 0;
        }
        s_p2Shadow = 0;
    }
}

/* Border BREAK (hit or bomb-press during the border). FUN_00441bd0 is __thiscall(player,
 * int flag) — ECX=player, ONE stack arg (the 0/1 flag), `ret 4` (verified in-binary). It
 * is called param-relative for whichever player was hit / bombed; we propagate the pop to
 * the partner so the one team border ends for both. No bonus either way (vanilla: breaking
 * forfeits it). The flag is forwarded so the callee's stack stays balanced. */
static void __fastcall HookedBorderBreak(void *self, void *edx, int flag)
{
    s_origBorderBreak(self, edx, flag);
    if (!s_p2TeamBorder || s_borderMirror) return;
    void *p2 = (void *)s_p2;
    if (!p2) return;
    void *other = NULL;
    if ((uint32_t)self == ADDR_PLAYER_BASE) other = p2;
    else if (self == p2)                    other = (void *)ADDR_PLAYER_BASE;
    if (other && *(unsigned char *)((char *)other + OFF_STATE) == 4) {
        s_borderMirror = 1;
        s_origBorderBreak(other, edx, flag);        /* a pop by either pops both */
        s_borderMirror = 0;
    }
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
            if (s_p2Ghost) p2in = GhostAutoInput(p2);  /* ghost auto-wanders, no shot/bomb */

            /* P2's bomb is its own spell card (spent from P2's separate bomb count
             * via the resource field-swap below) and is NOT masked. ZUN's bomb handler
             * casts a spell outside the border, and DURING the cherry border it just
             * pops the border early instead (no spell, no bomb spent) — the team-border
             * break hook then pops P1's half too. The cherry border never reads the
             * bomb bit; it auto-fires off the shared Cherry+ gauge. */
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

            /* Drive P2's ringless shadow border to track P1's (enter/sync/retire) BEFORE
             * P2's update, so P2 updates in the correct border state. */
            UpdateTeamBorder(p2);

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
             * flag. Cancel it (P1 plays on), drop P2 into ghost mode, and drop
             * the guaranteed last-life 1up at the death spot. */
            if (!s_p2Ghost && !goBefore && *ADDR_GAMEOVER) {
                *ADDR_GAMEOVER = goBefore;          /* undo P2-caused game-over */
                s_p2Ghost = 1;
                s_reviveFrames = 0;
                DropOneUp(p2);
                Log("P2 out of lives -> GHOST mode (auto-wander; graze %d frames to revive)",
                    REVIVE_FRAMES);
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

            /* graze resurrection: the shared struct holds P1's values here
             * (restored above), so the life donation can write it directly */
            if (s_p2Ghost) ReviveByGraze(p2);

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
    if (MH_CreateHook(ADDR_COLLIDE_GRAZE,  (LPVOID)&HookedGraze,         (LPVOID*)&s_origGraze)         != MH_OK) return 0;
    if (MH_CreateHook(ADDR_ECL_INTERP,     (LPVOID)&HookedEclInterp,     (LPVOID*)&s_origEclInterp)     != MH_OK) return 0;
    if (MH_CreateHook(ADDR_COLLECT_OVERLAP,(LPVOID)&HookedCollectOverlap,(LPVOID*)&s_origCollectOverlap) != MH_OK) return 0;
    if (MH_CreateHook(ADDR_ITEM_LOOP,      (LPVOID)&HookedItemLoop,      (LPVOID*)&s_origItemLoop)      != MH_OK) return 0;
    if (MH_CreateHook(ADDR_BORDER_BREAK,   (LPVOID)&HookedBorderBreak,   (LPVOID*)&s_origBorderBreak)   != MH_OK) return 0;
    if (MH_EnableHook(ADDR_PLAYER_UPDATE)  != MH_OK) return 0;
    if (MH_EnableHook(ADDR_PLAYER_DRAW)    != MH_OK) return 0;
    if (MH_EnableHook(ADDR_COLLIDE_BULLET) != MH_OK) return 0;
    if (MH_EnableHook(ADDR_COLLIDE_LASER)  != MH_OK) return 0;
    if (MH_EnableHook(ADDR_COLLIDE_GRAZE)  != MH_OK) return 0;
    if (MH_EnableHook(ADDR_ECL_INTERP)     != MH_OK) return 0;
    if (MH_EnableHook(ADDR_COLLECT_OVERLAP)!= MH_OK) return 0;
    if (MH_EnableHook(ADDR_ITEM_LOOP)      != MH_OK) return 0;
    if (MH_EnableHook(ADDR_BORDER_BREAK)   != MH_OK) return 0;
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
            "F4=team-border, F5=boss-HP-scale, F6=sep-resources, F7=shot-damage, F8=killable, "
            "F9=spawn, F10=despawn, F11=revive.");
        if (!InstallHooks())
            MessageBoxA(NULL, "th07_coop: hook install failed (wrong build/addresses?)",
                        "th07_coop", MB_ICONERROR);
        else
            Log("hooks installed (update @0x441fb0, draw @0x4420b0, "
                "collide-bullet @0x43e260, collide-laser @0x43e6b0, graze @0x43e3b0, "
                "ecl-interp @0x424290, collect-overlap @0x43e4e0, item-loop @0x432990, "
                "border-break @0x441bd0)");
        break;
    case DLL_PROCESS_DETACH:
        if (s_log) { Log("detach"); fclose(s_log); }
        MH_Uninitialize();
        break;
    }
    return TRUE;
}
