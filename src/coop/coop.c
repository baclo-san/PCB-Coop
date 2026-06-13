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
 *   end twice as fast. We detour the player-shot DAMAGE function (FUN_0043d9e0)
 *   and divide only its returned damage by the active player count (floor 1).
 *   The original runs first, so shot consumption + hit sparks happen exactly
 *   once. This catches EVERY enemy uniformly — the previous ECL set-life cap
 *   scaling (FUN_00424290, +0xd30) only ever caught popcorn; the 2026-06-12
 *   eclhp log proved the stage-1 midboss's HP never passes that path. Auto-arms
 *   when P2 is live; F5 toggles off. See docs/th07_boss_hp_scaling.md.
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
 * RESURRECTION (EoSD-mod style, user-specified 2026-06-12) — SYMMETRIC: EITHER
 *   player dying on their last life becomes a GHOST — invulnerable, no input,
 *   semi-transparent (half-alpha tint), drifting slowly (~1/3 focus speed,
 *   position driven directly) inside a band in the lower playfield, bouncing
 *   between the side walls. The survivor revives it by GRAZING the ghost
 *   (within 24px, focus state free) for 90 consecutive frames, then CONFIRMING
 *   with one focus release. While channeling, the real graze-credit leaf
 *   (FUN_0043eb90) fires every 6 frames for authentic graze SFX/spark feedback
 *   — with its stat effects (graze counters/score/bonus accumulator)
 *   snapshot-restored so nothing rises. The revive DONATES one of the
 *   survivor's lives — or is FREE when the survivor has no spare extends.
 *   Revive values: NO bonus lives (0 spares), bombs = the stock held at death,
 *   power = whatever the normal death drop left. Dying on the LAST life drops
 *   a guaranteed 1-UP at the death spot (tracked last-alive position; ZUN's
 *   item spawner FUN_004326f0, type 5). F11 = debug instant-revive.
 *
 * PHANTOM SPARE + GAME OVER: ZUN's lives==0 death path means game over (and
 *   emits useless-without-continues full-power items, and continue-resets the
 *   resources). So while co-op is active, a player at 0 spares gets a phantom
 *   spare swapped in around every update — the engine then processes any death
 *   as a NORMAL death (partial power drop, vanilla respawn) and never ends the
 *   game on its own. We detect the consumed phantom, take the player into
 *   ghost mode, and declare GAME OVER ourselves only when a player goes down
 *   while the partner is already a ghost (both dead at once).
 *
 * LIFE SHARING (EoSD-mod mechanic, same session): two LIVE players graze each
 *   other (same 24px / 90 frames channel, same feedback) with NEITHER shooting;
 *   the donor then confirms with one focus release -> loses a life, and a 1up
 *   pops out ~48px ABOVE the donor (plus the item's built-in upward pop) so the
 *   partner can catch it. Pickup is deliberately universal: if the donor
 *   re-eats their own 1up, the extend refunds the donation — net zero.
 *
 * Controls (in-stage):
 *   F9  = spawn P2 (clone of P1, offset +24px X). Only when a char is loaded.
 *   F10 = despawn P2 (stop piggyback + free).
 *   F8  = toggle P2 killability (default ON).
 *   F7  = legacy: transfer P2 shots into P1's array (default OFF — P2's shots
 *         now live in its own array, swept by the damage hook; the transfer
 *         orphaned MarisaB's laser slot-pointers and killed the lasers).
 *   F6  = toggle P2 separate resources (default ON).
 *   F5  = toggle boss/enemy HP scaling (default ON).
 *   F4  = toggle synced cherry team border (default ON).
 *   F3  = toggle P2's shot TYPE (A <-> B within its character) — P2 gets its
 *         own .sht pair + bomb callbacks (charselect stage 1; live re-apply OK).
 *   F2  = cycle P2's CHARACTER (Reimu->Marisa->Sakuya, charselect stage 2):
 *         loads that char's anm into a spare slot and swaps the 0x400-range
 *         sprite/script tables to P2 around its update+draw (live re-apply OK).
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
#define OFF_TINT      0x1b8        /* u32 sprite tint 0xAARRGGBB (update rewrites it) */
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

/* Tier-1 boss/enemy HP scaling — DAMAGE-side lever. The first attempt scaled
 * the per-phase max-life cap (+0xd30) at the ECL set-life opcode (FUN_00424290),
 * but the 2026-06-12 eclhp diagnostic proved only popcorn (cap 60) ever passes
 * that path — the stage-1 midboss's HP is set elsewhere and was never scaled.
 * So we scale at the other end: FUN_0043d9e0 sweeps the player's shot array
 * against a target box and RETURNS the total damage this frame (the per-enemy
 * damage-apply loop FUN_00420620:12822 subtracts the return from HP). Dividing
 * only the return catches EVERY enemy uniformly — no matter how its HP was set —
 * while shot consumption + hit sparks still happen exactly once. Both players'
 * shots flow through P1's array (shot transfer), so the divisor halves the
 * TEAM's doubled DPS back to vanilla pacing. See docs/th07_boss_hp_scaling.md §5. */
#define ADDR_DAMAGE ((LPVOID)0x0043d9e0) /* __thiscall(player, pos, size, outf) */

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

/* player SHOT array (in the player struct). Shots are created/moved/drawn
 * param-relative, so P2's shots live in P2's OWN array and the damage hook
 * sweeps it (re-invoke with ECX=P2). The old TRANSFER into P1's array is OFF
 * by default (F7 re-enables for A/B): it broke MarisaB's lasers — lasers ARE
 * shot slots, but they're maintained through owner-side slot POINTERS
 * (player+0x169d0/+0x169e0/+0x169f0, PCBdecomp.c:25665 FUN_0043d2f0); moving
 * the slot orphaned the pointer and the laser died instantly. */
#define OFF_SHOTS        0x2444    /* first shot slot                           */
#define SHOT_STRIDE      0x364     /* bytes per slot                            */
#define SHOT_COUNT       0x60      /* 96 slots                                  */
#define OFF_SHOT_ACTIVE  0x34a     /* u16 in slot; !=0 = active                 */

#define P2_SPAWN_OFFSET_X  24.0f

typedef int (__fastcall *PlayerFn_t)(void *self);
/* FUN_0043d9e0: __thiscall(player, float *pos, float *size, int *out_flag),
 * returns total shot damage vs the target box. Modeled as __fastcall. */
typedef int (__fastcall *DamageFn_t)(void *self, void *edx,
                                     float *pos, float *size, int *out_flag);

static PlayerFn_t s_origUpdate = NULL;
static PlayerFn_t s_origDraw   = NULL;
static CollideBulletFn_t s_origCollideBullet = NULL;
static CollideBulletFn_t s_origGraze         = NULL;   /* FUN_0043e3b0, same signature */
static CollideLaserFn_t  s_origCollideLaser  = NULL;
static CollectOverlapFn_t s_origCollectOverlap = NULL;
static DamageFn_t        s_origDamage        = NULL;
static ItemLoopFn_t      s_origItemLoop       = NULL;

static volatile void *s_p2   = NULL;   /* P2 object base, NULL until spawned     */
static int   s_p2Killable = 1;         /* on by default; F8 toggles              */
static unsigned char s_p2PrevState = 0;/* for death-transition logging           */
static int   s_prevF9 = 0, s_prevF10 = 0, s_prevF8 = 0, s_prevF7 = 0, s_prevF6 = 0, s_prevF11 = 0, s_prevF5 = 0, s_prevF4 = 0, s_prevF3 = 0, s_prevF2 = 0;

/* P2's own LIVES + BOMBS + POWER, field-swapped into the shared struct around P2's
 * update; ZUN's anti-tamper checksum is re-healed afterward (see the heal below).
 * Lives separation is safe now: the life-loss decrement (FUN_0042d5cd) recomputes
 * the checksum, which the heal covers, and P2 running OUT of lives is intercepted
 * into ghost mode instead of triggering team game-over. */
static int   s_p2SepRes = 1;           /* separate lives/bombs/power on; F6 toggles*/
static float s_p2Lives = 0.f, s_p2Bombs = 0.f, s_p2Power = 0.f;
static int   s_p2PrevLives = -1, s_p2PrevBombs = -1, s_p2PrevPower = -1;
static int   s_p2Carry = 0;            /* next SpawnP2 keeps s_p2* (stage transition) */

/* GHOST MODE (symmetric, both players): the PHANTOM SPARE in HookedUpdate
 * keeps ZUN's death commit from ever seeing 0 lives, so a last-life death runs
 * the NORMAL death path (partial power drop, no full-power items, no continue
 * reset, no game-over flag); we then detect the consumed phantom and turn that
 * player into an auto-wandering, invulnerable, non-shooting "ghost" until the
 * partner graze-revives it (EoSD-mod style — see the header). Collision is
 * skipped for a ghost so it can't re-die. GAME OVER is OURS to declare: only
 * when a player goes down while the partner is already a ghost. */
static int   s_p2Ghost = 0;
static int   s_p1Ghost = 0;            /* symmetric: P1 ghosts too (P2 revives it) */
static int   s_runOver = 0;            /* both players downed -> we declared game over */
static int   s_ghostDirX = 1, s_ghostDirY = 1;  /* auto-wander bounce direction */
static int   s_reviveFrames = 0;       /* consecutive frames the ghost was grazed */
static int   s_p1PrevFocus = 0, s_p2PrevFocus = 0; /* focus-release edge tracking */
/* Last ALIVE position + bomb stock, tracked per frame (state 0/4). The death FSM
 * resets pos to center and refills bombs before we see the last-life death, so
 * the 1up drop spot and the revive bomb count must come from these. */
static float s_p2AlivePos[3] = {192.f, 384.f, 0.f};
static float s_p1AlivePos[3] = {192.f, 384.f, 0.f};
static float s_p1AliveBombs = 0.f, s_p2AliveBombs = 0.f;

/* ---- P2 focus ring ----
 * The focus visual is effect type 0x18, spawned by the player's option-mode
 * machine via FUN_0041c610(mgr, 0x18, &pos, slotArg=2, 1, white) into the FIXED
 * player-effect slot 402 (slot = 400+slotArg; PCBdecomp.c:26412), handle kept at
 * player+0x9d8, killed on focus release by writing 1 into effect+0x1c6 (26420).
 * Its per-frame updater FUN_0041abe0 (10257) snaps the effect to the STATIC P1
 * position (DAT_004be408) — so P2's vanilla spawn both fights P1's ring (same
 * slot) AND can never follow P2. We defang P2's vanilla spawn each frame and run
 * our own type-0x18 ring in FREE player-effect slot 406, re-pinning its position
 * to P2 after updates and again at draw time (the updater keeps snapping it back
 * to P1; last write before the draw wins). */
#define ADDR_EFFECT_MGR   ((void *)0x012fe250) /* static; slots at +0x1c, stride 0x2d8 */
#define FX_TYPE_FOCUS     0x18
#define FX_SLOT_P2_FOCUS  6        /* player-effect slot 406 (402=focus, 404=border) */
#define OFF_FX_KILL       0x1c6    /* u16: 1 = fade out & die                      */
#define OFF_FX_TYPE       0x2cd    /* u8: effect type byte                         */
#define OFF_FX_POS        0x24c    /* float[3] world position                      */
#define OFF_PLAYER_FX     0x9d8    /* player: handle of its own focus effect       */
/* __thiscall(mgr, type, pos, slotArg, a5, color) modelled as __fastcall */
typedef void *(__fastcall *EffectSpawnFn_t)(void *mgr, void *edx, int type,
                                            float *pos, int slotArg, int a5,
                                            uint32_t color);
#define ADDR_EFFECT_SPAWN ((LPVOID)0x0041c610)
static EffectSpawnFn_t s_origEffectSpawn = NULL;
static void *s_p2FocusFx = NULL;
static volatile int s_inP2Update = 0;   /* inside s_origUpdate(P2) right now */

/* Redirect P2's vanilla focus-ring spawn (type 0x18, slot 2 = P1's slot 402)
 * into P2's own slot 406. ZUN's option-mode machine then runs P2's ring
 * lifecycle natively via p2+0x9d8 — spawn on press, kill on release — and
 * P1's slot is never touched (the old kill-the-stray defang made P1's ring
 * blink whenever P2 tapped focus while P1 held it). */
static void *__fastcall HookedEffectSpawn(void *mgr, void *edx, int type,
                                          float *pos, int slotArg, int a5,
                                          uint32_t color)
{
    if (s_inP2Update && type == FX_TYPE_FOCUS && slotArg == 2)
        slotArg = FX_SLOT_P2_FOCUS;
    return s_origEffectSpawn(mgr, edx, type, pos, slotArg, a5, color);
}

static void KillP2FocusFx(void)
{
    if (!s_p2FocusFx) return;
    /* guard: only if the slot still holds a focus-type effect (stage transitions
     * recycle the effect array under us) */
    if (*(unsigned char *)((char *)s_p2FocusFx + OFF_FX_TYPE) == FX_TYPE_FOCUS)
        *(uint16_t *)((char *)s_p2FocusFx + OFF_FX_KILL) = 1;
    s_p2FocusFx = NULL;
}

/* ---- stage-start signal + on-screen text ----
 * The reliable stage-start signal is the LOGIC-FRAME COUNTER: FUN_00442cd0 is
 * the per-frame replay-record task (the netcode's input-inject seam) and
 * `*param` increments once per logic frame, restarting from 0 when a stage
 * load re-news the task object. A DECREASE in the counter = a real stage/game
 * start. (Two prior attempts failed: a >2s update-gap detector never fired —
 * PCB keeps ticking through transitions; and hooking FUN_00442c60 cycled P2
 * forever — that init RE-FIRES mid-stage, 12 rebuild cycles in one run. The
 * handoff's "runs once at game start" note was wrong.) */
#define ADDR_FRAME_TASK ((LPVOID)0x00442cd0)
typedef int (__fastcall *FrameTaskFn_t)(int *self);
static FrameTaskFn_t s_origFrameTask = NULL;
static int s_lastLogicFrame = 0x7fffffff;

/* ZUN's on-screen text: FUN_00402060(ascii_mgr, float pos[3], fmt, ...) —
 * vsprintf into a local buffer, then queue on the ascii manager (static at
 * 0x0134ce18); the manager's own draw task renders the queue each frame
 * (16px line height, sidebar coords — the HUD "%d/%d" point display uses
 * (496,176), PCBdecomp.c:17163). Queueing is render-state-independent; call
 * once per frame. Plain __cdecl varargs. */
typedef void (__cdecl *AsciiPrintFn_t)(void *mgr, float *pos, const char *fmt, ...);
#define ADDR_ASCII_PRINT ((AsciiPrintFn_t)0x00402060)
#define ADDR_ASCII_MGR   ((void *)0x0134ce18)

/* Graze credit leaf FUN_0043eb90: __thiscall(player, float *grazed_pos) — bumps
 * the graze counters, spawns the graze spark at the midpoint, queues the graze
 * SFX, +200 score. Called every few frames during the revive/share channel so
 * grazing a partner sounds and looks like real grazing — but the STAT effects
 * (graze counters, score, graze bonus accumulator) are snapshot/restored around
 * the call (user: feedback only, the counter must not rise). */
typedef void (__fastcall *GrazeCreditFn_t)(void *self, void *edx, float *pos);
#define ADDR_GRAZE_CREDIT ((GrazeCreditFn_t)0x0043eb90)
#define RES_SCORE        0x04      /* int: score (graze credit adds 200)        */
#define RES_GRAZE_CUR    0x14      /* int: graze counter (HUD, cap 9999)        */
#define RES_GRAZE_TOTAL  0x18      /* int: total graze (cap 999999)             */
#define ADDR_GRAZE_BONUS_ACC ((volatile int *)0x012fe0d0) /* graze score-bonus accumulator */

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
#define AUTO_SPAWN_AFTER 30            /* frames of P1 in state 0 after the stage
                                          fly-in, then spawn P2 (user: both players
                                          up together at stage start) */

/* HOMING TARGET: the per-enemy update writes the frame's chosen homing target
 * position ONLY into the static P1's field (absolute DAT_004bff00/04 =
 * 0x4bdad8+0x2428, PCBdecomp.c:12908-12932). Every homing READER is
 * param-relative — bomb orbs (6061) and homing shots (25226) steer toward
 * player+0x2428/+0x242c, with <= -100 as the "no target" sentinel that the
 * player's own update re-arms each frame. P2's copy was therefore never
 * filled -> its bomb orbs never homed. coop.c copies P1's target into P2 at
 * draw time (the bomb fns run from the player draw). Note both players home
 * at P1's chosen enemy (the chooser ranks by distance to P1) — close enough.
 *
 * The block is bigger than just the homing target. The enemy update keeps a
 * whole absolutely-addressed target group at static P1 +0x2428..+0x2443:
 *   +0x2428 float[3]  homing target x,y,z   (0x4bff00/04/08)
 *   +0x2434 float[3]  SakuyaA AIM target    (0x4bff0c/10/14) — only filled
 *           when global char id DAT_0062f645 == 2 (Sakuya), enemy in the
 *           upward ~60° cone from P1 (PCBdecomp.c:12913-12943); consumed
 *           param-relatively by the aimed-shot spawn cb FUN_0043c0d0 (25176)
 *   +0x2440 int       target-valid flag     (0x4bff18)
 * Mirror the whole 0x1c-byte block or SakuyaA's focused shot fires straight
 * for P2 (round-11 bug). */
#define OFF_HOMING_TGT 0x2428          /* base of the target block               */
#define HOMING_TGT_LEN 0x1c            /* homing xyz + aim xyz + valid flag      */

/* ---- P2 SHOT-TYPE SELECT (charselect stage 1: same character, own A/B) ----
 * The engine keeps the chosen loadout in three globals and a per-player cache:
 *   0x62f645 char id (0 Reimu / 1 Marisa / 2 Sakuya), 0x62f646 type (0=A 1=B),
 *   0x62f647 combined sel 0-5 (= char*2 + type).
 * The player init FUN_004423e0 (PCBdecomp.c:27340) consumes them ONCE:
 *   - loads data/ply{00,01,02}{a,b}[s].sht via FUN_00442b70(ECX=&player+0xb7e70
 *     / +0xb7e74, EDX=nameTbl[sel]) — name tables at 0x49f530 (unfocused) and
 *     0x49f548 (focused; disasm-verified at 0x442400/0x442429). The loader
 *     relocates the buffer and resolves per-shot-entry cb indices to fn ptrs.
 *   - installs 4 per-sel BOMB cbs (player+0x16a3c..48) from the table at
 *     0x49ec50 (6 sels x 4: unfoc-bomb upd/draw, foc-bomb upd/draw).
 *   - bakes speed/hitbox stats from the unfocused .sht header (+0xc/+0x10/+8).
 * EVERY gameplay consumer is then param-relative — the firing iterator
 * FUN_0043d160 (25600) walks player+0xb7e70/74 — so P2 gets its own loadout by
 * rewriting exactly these fields. Char-gated GLOBAL branches that run inside
 * the player's own code path (e.g. MarisaB's border fire-suppress, 25804) are
 * covered by swapping the three globals around P2-context engine calls.
 * Same character only for now: a different char needs its own .anm (the init
 * loads data/player0N.anm into the single ANM slot 10 — P2 would need a free
 * slot + a script-id remap; that is charselect stage 2). */
#define ADDR_CHAR_ID      ((volatile unsigned char *)0x0062f645)
#define ADDR_TYPE_ID      ((volatile unsigned char *)0x0062f646)
#define ADDR_SEL_ID       ((volatile unsigned char *)0x0062f647)
#define ADDR_SHT_NAMES_N  ((const char *const *)0x0049f530)  /* by sel, unfocused */
#define ADDR_SHT_NAMES_F  ((const char *const *)0x0049f548)  /* by sel, focused   */
#define ADDR_SEL_NAMES    ((const char *const *)0x0049f4ec)  /* "ReimuA".."SakuyaB" */
#define ADDR_BOMB_CB_TBL  ((void *const *)0x0049ec50)        /* [sel*4 + 0..3]    */
typedef int (__fastcall *ShtLoadFn_t)(void **out, const char *name); /* 0 = ok */
#define ADDR_SHT_LOAD     ((ShtLoadFn_t)0x00442b70)
#define OFF_SHT_UNFOC     0xb7e70      /* void*: unfocused .sht buffer            */
#define OFF_SHT_FOC       0xb7e74      /* void*: focused .sht buffer              */
#define OFF_BOMB_CB       0x16a3c     /* 4 consecutive cb ptrs                    */
#define OFF_SPD_UNF_CUR   0x990
#define OFF_SPD_UNF       0x994       /* = sht[+0xc]/2                            */
#define OFF_SPD_FOC_CUR   0x99c
#define OFF_SPD_FOC       0x9a0       /* = sht[+0x10]/2                           */
#define OFF_HITBOX        0x23f8      /* = sht[+0x8]                              */
static int   s_p2Sel = -1;             /* P2's sel 0-5; -1 = mirror P1. F3 toggles A/B */
static void *s_shtCache[6][2];         /* loaded .sht pairs (game heap, kept for life) */
static unsigned char s_selSaved[3];    /* global-swap save slots                  */
static int   s_selSwapped = 0;
static int   s_allowDiffChar = 0;      /* F2 enables cycling P2 to a DIFFERENT char */

/* ---- DIFFERENT CHARACTER for P2 (charselect stage 2) ----
 * The sprites/animation live in an ANM file (data/player0{0,1,2}.anm) the init
 * loads into anm slot 10 at global id base 0x400. The anm manager keeps two
 * parallel global tables (one mgr, base ptr at *0x4b9e44):
 *   scripts:  mgr+0x28ef0 + id*4         (a script-bytecode pointer per id)
 *   sprites:  mgr+0x60    + id*0x40      (a 0x40-byte sprite def per id)
 *   slots:    mgr+0x2def0 + slot*0xc     (per-slot file ptr; 0 = slot free)
 * with id space 0..0x9ff, the player char occupying ~0x400..0x481+.
 *
 * Shifting P2 to a free id base is NOT viable: the player movement update
 * RE-BINDS the body sprite every tilt change to HARD-CODED ids 0x400..0x404
 * (PCBdecomp.c:26323-26341,26759, reading mgr+0x29ef0..0x29f00 directly), and
 * shots bind script ids straight from the .sht — all in the 0x400 range, in
 * code shared with P1. So instead BOTH chars load at base 0x400 into separate
 * slots, and we SWAP the 0x400-range table entries to P2's char around P2's
 * update + draw (the only windows P2's char-specific binds/anim run in). A
 * bind resolves to a stored pointer once (FUN_0044ea20 writes spriteobj+0x77),
 * so a shot/body bound while the swap is active stays P2's. Outside the window
 * the tables are P1's, so P1 + dialogue faces (ids 0x4a0+) are untouched.
 * We capture P2's EXACT id set by snapshot-diffing the tables across the load,
 * so the swap only touches ids P2 actually defines. */
#define ADDR_ANM_MGR_PP   ((volatile uint32_t *)0x004b9e44) /* -> anm mgr base   */
#define ANM_SCRIPT_TBL    0x28ef0      /* mgr+: script ptr per id                 */
#define ANM_SPRITE_TBL    0x60         /* mgr+: sprite def per id, stride 0x40    */
#define ANM_SPRITE_STRIDE 0x40
#define ANM_SLOT_TBL      0x2def0      /* mgr+: per-slot file ptr (0=free), 0xc   */
#define ANM_ID_LO         0x400        /* scan window for the player char's ids   */
#define ANM_ID_HI         0x4a0        /* exclusive; below the in-stage face ids  */
#define ANM_MAX_IDS       (ANM_ID_HI - ANM_ID_LO)
/* FUN_0044df90 is a TRUE __thiscall: ECX=mgr, then (slot,file,base) on the
 * stack, callee-cleaned — not the ECX/EDX __fastcall the other hooks model. */
typedef int (__attribute__((thiscall)) *AnmLoadFn_t)(void *mgr, int slot,
                                                      const char *file, int base);
#define ADDR_ANM_LOAD_FN  ((AnmLoadFn_t)0x0044df90)
typedef void (__attribute__((thiscall)) *AnmFreeFn_t)(void *mgr, int slot);
#define ADDR_ANM_FREE_FN  ((AnmFreeFn_t)0x0044e4e0)

static int      s_p2AnmChar = -1;      /* char id whose anm is loaded for P2; -1 none */
static int      s_p2AnmSlot = -1;      /* anm slot holding P2's char anm              */
static int      s_p2AnmActive = 0;     /* P2 currently uses a different-char anm       */
static int      s_p2IdCount = 0;       /* how many ids P2's char defines in the window */
static uint16_t s_p2Ids[ANM_MAX_IDS];                 /* the ids P2 defines            */
static uint32_t s_p2Script[ANM_MAX_IDS];              /* P2's script ptr per id        */
static unsigned char s_p2Sprite[ANM_MAX_IDS][ANM_SPRITE_STRIDE]; /* P2's sprite def    */
static int      s_anmSwapped = 0;

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
static int s_shotXfer = 0;            /* OFF: P2's shots stay in P2's array (the
                                         damage hook sweeps it; transfer broke
                                         MarisaB lasers). F7 re-enables for A/B. */
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

static void FreeP2CharAnm(void);

/* Load P2's character anm into its own slot at base 0x400, then snapshot-diff
 * the script + sprite tables to learn exactly which ids it defines, and restore
 * P1's tables. Returns 1 on success. The anm buffer + slot stay allocated for
 * the swap; freed in FreeP2CharAnm. */
static int LoadP2CharAnm(int charId)
{
    static const char *const anmNames[3] =
        { "data/player00.anm", "data/player01.anm", "data/player02.anm" };
    uint32_t mgr = *ADDR_ANM_MGR_PP;
    int slot, id, n = 0;
    char *scr, *spr, *slt;

    if (charId < 0 || charId > 2 || !mgr) return 0;
    if (s_p2AnmChar == charId && s_p2IdCount > 0) return 1;   /* already loaded */

    FreeP2CharAnm();

    scr = (char *)(mgr + ANM_SCRIPT_TBL);
    spr = (char *)(mgr + ANM_SPRITE_TBL);
    slt = (char *)(mgr + ANM_SLOT_TBL);

    /* pick a free anm slot high in the table (menu/result/title slots, free
     * during a stage); never slot 10 (P1's live char) or the low engine slots */
    slot = -1;
    for (id = 0x31; id >= 0x14; id--) {
        if (id == 10) continue;
        if (*(uint32_t *)(slt + id * 0xc) == 0) { slot = id; break; }
    }
    if (slot < 0) { Log("P2 char anm: no free slot"); return 0; }

    /* snapshot P1's tables across the window, then load P2's char OVER base
     * 0x400 (overwrites only the ids P2's anm defines), diff, restore P1's. */
    {
        uint32_t p1Script[ANM_MAX_IDS];
        unsigned char p1Sprite[ANM_MAX_IDS][ANM_SPRITE_STRIDE];
        int i, rc;
        for (i = 0; i < ANM_MAX_IDS; i++) {
            p1Script[i] = *(uint32_t *)(scr + (ANM_ID_LO + i) * 4);
            memcpy(p1Sprite[i], spr + (ANM_ID_LO + i) * ANM_SPRITE_STRIDE,
                   ANM_SPRITE_STRIDE);
        }
        rc = ADDR_ANM_LOAD_FN((void *)mgr, slot, anmNames[charId], ANM_ID_LO);
        if (rc != 0) {
            Log("P2 char anm: load %s -> slot %d FAILED rc=%d",
                anmNames[charId], slot, rc);
            /* the loader cleared the slot first; restore our snapshot just in
             * case a partial write happened, then bail */
            for (i = 0; i < ANM_MAX_IDS; i++) {
                *(uint32_t *)(scr + (ANM_ID_LO + i) * 4) = p1Script[i];
                memcpy(spr + (ANM_ID_LO + i) * ANM_SPRITE_STRIDE, p1Sprite[i],
                       ANM_SPRITE_STRIDE);
            }
            return 0;
        }
        for (i = 0; i < ANM_MAX_IDS; i++) {
            uint32_t curS = *(uint32_t *)(scr + (ANM_ID_LO + i) * 4);
            int sprChanged = memcmp(spr + (ANM_ID_LO + i) * ANM_SPRITE_STRIDE,
                                    p1Sprite[i], ANM_SPRITE_STRIDE) != 0;
            if (curS != p1Script[i] || sprChanged) {
                s_p2Ids[n] = (uint16_t)(ANM_ID_LO + i);
                s_p2Script[n] = curS;
                memcpy(s_p2Sprite[n],
                       spr + (ANM_ID_LO + i) * ANM_SPRITE_STRIDE,
                       ANM_SPRITE_STRIDE);
                n++;
            }
            /* restore P1's live tables */
            *(uint32_t *)(scr + (ANM_ID_LO + i) * 4) = p1Script[i];
            memcpy(spr + (ANM_ID_LO + i) * ANM_SPRITE_STRIDE, p1Sprite[i],
                   ANM_SPRITE_STRIDE);
        }
    }

    s_p2AnmChar  = charId;
    s_p2AnmSlot  = slot;
    s_p2IdCount  = n;
    Log("P2 char anm: %s -> slot %d, %d ids captured (0x%x..0x%x)",
        anmNames[charId], slot, n,
        n ? s_p2Ids[0] : 0, n ? s_p2Ids[n - 1] : 0);
    return n > 0;
}

static void FreeP2CharAnm(void)
{
    uint32_t mgr = *ADDR_ANM_MGR_PP;
    if (s_p2AnmSlot >= 0 && mgr) {
        ADDR_ANM_FREE_FN((void *)mgr, s_p2AnmSlot);
        Log("P2 char anm: freed slot %d", s_p2AnmSlot);
    }
    s_p2AnmChar = -1;
    s_p2AnmSlot = -1;
    s_p2IdCount = 0;
    s_p2AnmActive = 0;
    s_anmSwapped = 0;
}

/* Exchange the 0x400-range table entries between P1's (live) and P2's captured
 * char for exactly the ids P2 defines. enter=1 installs P2, enter=0 restores
 * P1. Re-entrancy-guarded; only acts while P2 has a different-char anm. */
static void SwapAnm(int enter)
{
    uint32_t mgr = *ADDR_ANM_MGR_PP;
    char *scr, *spr;
    int i;
    if (!s_p2AnmActive || !mgr || s_p2IdCount == 0) return;
    if (enter == s_anmSwapped) return;
    scr = (char *)(mgr + ANM_SCRIPT_TBL);
    spr = (char *)(mgr + ANM_SPRITE_TBL);
    for (i = 0; i < s_p2IdCount; i++) {
        int id = s_p2Ids[i];
        uint32_t *sScr = &s_p2Script[i];
        uint32_t *tScr = (uint32_t *)(scr + id * 4);
        unsigned char *tSpr = (unsigned char *)(spr + id * ANM_SPRITE_STRIDE);
        uint32_t tmp; unsigned char tmpS[ANM_SPRITE_STRIDE];
        tmp = *tScr; *tScr = *sScr; *sScr = tmp;            /* swap script ptr  */
        memcpy(tmpS, tSpr, ANM_SPRITE_STRIDE);
        memcpy(tSpr, s_p2Sprite[i], ANM_SPRITE_STRIDE);
        memcpy(s_p2Sprite[i], tmpS, ANM_SPRITE_STRIDE);     /* swap sprite def  */
    }
    s_anmSwapped = enter;
}

/* Swap the three selection globals to P2's identity around a P2-context engine
 * call so char/type-gated branches (MarisaB fire-suppress during a border,
 * SakuyaB checks, ...) see P2, not P1. No-op when P2 mirrors P1. */
static void SwapSelGlobals(int enter)
{
    if (enter) {
        int sel = (s_p2Sel < 0) ? *ADDR_SEL_ID : s_p2Sel;
        if (sel == *ADDR_SEL_ID) return;
        s_selSaved[0] = *ADDR_CHAR_ID;
        s_selSaved[1] = *ADDR_TYPE_ID;
        s_selSaved[2] = *ADDR_SEL_ID;
        *ADDR_CHAR_ID = (unsigned char)(sel / 2);
        *ADDR_TYPE_ID = (unsigned char)(sel & 1);
        *ADDR_SEL_ID  = (unsigned char)sel;
        s_selSwapped = 1;
    } else if (s_selSwapped) {
        *ADDR_CHAR_ID = s_selSaved[0];
        *ADDR_TYPE_ID = s_selSaved[1];
        *ADDR_SEL_ID  = s_selSaved[2];
        s_selSwapped = 0;
    }
}

/* Install P2's selected loadout into its struct: own .sht pair (loaded through
 * the engine's own loader, cached per selection), the 4 per-sel bomb cbs, and
 * the .sht-header-derived stats the init bakes. Re-applying mid-stage is safe:
 * live shots/lasers keep cb pointers into the previously cached buffer, which
 * stays valid for the process lifetime. Called from SpawnP2 and the F3 toggle. */
static void ApplyP2Selection(void *p2)
{
    char *pp = (char *)p2, *p1 = (char *)ADDR_PLAYER_BASE;
    int p1sel = *ADDR_SEL_ID;
    int sel = (s_p2Sel < 0) ? p1sel : s_p2Sel;
    int k;

    /* stage 1 (default): clamp to P1's character, keep the chosen A/B type.
     * stage 2 (F2): allow a different character — load its anm for the swap. */
    if (!s_allowDiffChar && sel / 2 != p1sel / 2)
        sel = (p1sel & ~1) | (sel & 1);
    s_p2Sel = sel;

    /* different-character path: ensure P2's char anm is loaded + arm the swap;
     * a same-char selection retires it (P2 shares P1's live tables). */
    if (sel / 2 != p1sel / 2) {
        if (LoadP2CharAnm(sel / 2)) {
            s_p2AnmActive = 1;
        } else {
            Log("P2 loadout: char anm load failed -> falling back to P1's char");
            sel = (p1sel & ~1) | (sel & 1);
            s_p2Sel = sel;
            FreeP2CharAnm();
        }
    } else {
        FreeP2CharAnm();
    }

    if (sel == p1sel) {
        /* mirror P1's loadout (also the path BACK after a toggle away) */
        memcpy(pp + OFF_SHT_UNFOC, p1 + OFF_SHT_UNFOC, 8);
        for (k = 0; k < 4; k++)
            *(void **)(pp + OFF_BOMB_CB + 4 * k) =
                *(void **)(p1 + OFF_BOMB_CB + 4 * k);
        *(float *)(pp + OFF_SPD_UNF) = *(float *)(p1 + OFF_SPD_UNF);
        *(float *)(pp + OFF_SPD_UNF_CUR) = *(float *)(p1 + OFF_SPD_UNF_CUR);
        *(float *)(pp + OFF_SPD_FOC) = *(float *)(p1 + OFF_SPD_FOC);
        *(float *)(pp + OFF_SPD_FOC_CUR) = *(float *)(p1 + OFF_SPD_FOC_CUR);
        *(uint32_t *)(pp + OFF_HITBOX) = *(uint32_t *)(p1 + OFF_HITBOX);
        Log("P2 loadout: %s (mirrors P1)", ADDR_SEL_NAMES[sel]);
        return;
    }

    if (!s_shtCache[sel][0]) {
        void *bufN = NULL, *bufF = NULL;
        if (ADDR_SHT_LOAD(&bufN, ADDR_SHT_NAMES_N[sel]) != 0 ||
            ADDR_SHT_LOAD(&bufF, ADDR_SHT_NAMES_F[sel]) != 0) {
            Log("P2 loadout: .sht load FAILED (%s / %s) — keeping P1's",
                ADDR_SHT_NAMES_N[sel], ADDR_SHT_NAMES_F[sel]);
            s_p2Sel = p1sel;
            return;
        }
        s_shtCache[sel][0] = bufN;
        s_shtCache[sel][1] = bufF;
        Log("P2 loadout: loaded %s + %s", ADDR_SHT_NAMES_N[sel], ADDR_SHT_NAMES_F[sel]);
    }

    *(void **)(pp + OFF_SHT_UNFOC) = s_shtCache[sel][0];
    *(void **)(pp + OFF_SHT_FOC)   = s_shtCache[sel][1];
    for (k = 0; k < 4; k++)
        *(void **)(pp + OFF_BOMB_CB + 4 * k) = ADDR_BOMB_CB_TBL[sel * 4 + k];
    {
        char *sht = (char *)s_shtCache[sel][0];
        *(float *)(pp + OFF_SPD_UNF) = *(float *)(sht + 0xc) / 2.0f;
        *(float *)(pp + OFF_SPD_UNF_CUR) = *(float *)(pp + OFF_SPD_UNF);
        *(float *)(pp + OFF_SPD_FOC) = *(float *)(sht + 0x10) / 2.0f;
        *(float *)(pp + OFF_SPD_FOC_CUR) = *(float *)(pp + OFF_SPD_FOC);
        *(uint32_t *)(pp + OFF_HITBOX) = *(uint32_t *)(sht + 8);
    }
    Log("P2 loadout: %s (P1 = %s)", ADDR_SEL_NAMES[sel], ADDR_SEL_NAMES[p1sel]);
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

    /* symmetric spawn: P1 steps left, P2 takes the right (user: both players
     * side by side around the spawn point at stage start) */
    *(float *)((char *)ADDR_PLAYER_BASE + OFF_POS_X) -= P2_SPAWN_OFFSET_X;
    *(float *)((char *)p2 + OFF_POS_X) += P2_SPAWN_OFFSET_X;

    /* the clone copied P1's focus-ring handle — P2 must not own P1's ring */
    *(uint32_t *)((char *)p2 + OFF_PLAYER_FX) = 0;

    ApplyP2Selection(p2);                   /* P2's own shot type (F3 toggles) */

    s_p2PrevState = *(unsigned char *)((char *)p2 + OFF_STATE);

    /* P2 gets its own LIVES + BOMBS + POWER (field-swapped around its update),
     * seeded from P1's current counts. Running out of lives -> ghost mode. */
    {
        uint32_t res = *ADDR_RES_PTR;
        if (s_p2Carry) {
            s_p2Carry = 0;          /* stage transition: P2 keeps its own resources */
        } else if (res) {
            s_p2Lives = *(float *)(res + RES_LIVES);
            s_p2Bombs = *(float *)(res + RES_BOMBS);
            s_p2Power = *(float *)(res + RES_POWER);
        }
        s_p2Ghost = 0;
        s_p1Ghost = 0;
        s_runOver = 0;
        s_reviveFrames = 0;
        s_p2AliveBombs = s_p2Bombs;
        memcpy(s_p2AlivePos, (char *)p2 + OFF_POS_X, sizeof(s_p2AlivePos));
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
    SwapAnm(0);                             /* ensure tables are P1's before free */
    FreeP2CharAnm();
    KillP2FocusFx();
    VirtualFree(p2, 0, MEM_RELEASE);
    Log("despawn: P2 freed");
}

/* Revive values (user spec 2026-06-12): NO bonus lives (0 spares — the revived
 * life IS the one you're playing on), bombs = the stock held when the player
 * DIED (set at ghost entry from the alive-tracked value; the vanilla respawn
 * refill is overridden), power = whatever the normal death drop left (the
 * phantom-spare system routes a last-life death through ZUN's NORMAL death
 * path, so the usual partial power drop already happened — nothing to do). */
static void ReviveP2(void)
{
    void *p2 = (void *)s_p2;
    if (!p2 || !s_p2Ghost) return;

    s_p2Ghost = 0;
    if (s_p2Lives < 0.f) s_p2Lives = 0.f;           /* 0 spares, never negative */

    /* set P2 into respawn-invulnerable state and place near the reviver (P1) */
    *(unsigned char *)((char *)p2 + OFF_STATE) = 3; /* respawn-invuln */
    /* ARM the invuln countdown — state 3 exits when +0x16a08 drops below 1,
     * counted toward the -999 target. Setting state=3 with a stale counter
     * left P2 flickering-invulnerable forever (user-observed; a border "cured"
     * it because state 4 rewrites these same multiplexed timer fields). */
    *(int *)((char *)p2 + OFF_BORDER_T0) = 240;         /* ~4s invuln */
    *(int *)((char *)p2 + OFF_BORDER_T1) = 0;
    *(int *)((char *)p2 + OFF_BORDER_T2) = 0xfffffc19;  /* count target -999 */
    *(float *)((char *)p2 + OFF_POS_X) = *(float *)((char *)ADDR_PLAYER_BASE + OFF_POS_X) + P2_SPAWN_OFFSET_X;
    *(float *)((char *)p2 + OFF_POS_Y) = *(float *)((char *)ADDR_PLAYER_BASE + OFF_POS_Y);
    *(uint32_t *)((char *)p2 + OFF_TINT) = 0xffffffffu;  /* drop the ghost tint */

    Log("P2 revived from GHOST: lives=%.0f bombs=%.0f power=%.0f pos=(%.1f,%.1f)",
        s_p2Lives, s_p2Bombs, s_p2Power,
        *(float *)((char *)p2 + OFF_POS_X), *(float *)((char *)p2 + OFF_POS_Y));
}

static void ReviveP1(void)
{
    void *p1 = (void *)ADDR_PLAYER_BASE;
    void *p2 = (void *)s_p2;
    if (!s_p1Ghost || !p2) return;

    s_p1Ghost = 0;
    /* lives stay as-is (0 spares); bombs were set to the death stock at ghost
     * entry; power is the post-drop value the normal death left in the struct */
    *(unsigned char *)((char *)p1 + OFF_STATE) = 3;
    *(int *)((char *)p1 + OFF_BORDER_T0) = 240;         /* arm the invuln countdown */
    *(int *)((char *)p1 + OFF_BORDER_T1) = 0;
    *(int *)((char *)p1 + OFF_BORDER_T2) = 0xfffffc19;
    *(float *)((char *)p1 + OFF_POS_X) = *(float *)((char *)p2 + OFF_POS_X) - P2_SPAWN_OFFSET_X;
    *(float *)((char *)p1 + OFF_POS_Y) = *(float *)((char *)p2 + OFF_POS_Y);
    *(uint32_t *)((char *)p1 + OFF_TINT) = 0xffffffffu;

    Log("P1 revived from GHOST: state=3 pos=(%.1f,%.1f)",
        *(float *)((char *)p1 + OFF_POS_X), *(float *)((char *)p1 + OFF_POS_Y));
}

/* ---- EoSD-style resurrection (user-specified 2026-06-12) ---- */

/* Ghost auto-movement: drift slowly inside a band in the lower playfield,
 * bouncing between the side walls and the band top/bottom. The first version
 * steered via input bits, which moves at the character's (unfocused) speed —
 * way too fast to graze (user test). So the ghost's position is driven
 * DIRECTLY (the hitbox and sprite both track +0x930, verified) at GHOST_SPEED,
 * with all input bits masked. Playfield ~384x448 world units; band raised
 * 50px from the first test per user feedback. */
#define GHOST_X_MIN   48.0f
#define GHOST_X_MAX  336.0f
#define GHOST_Y_TOP  302.0f
#define GHOST_Y_BOT  382.0f
#define GHOST_SPEED    0.8f            /* ~1/3 focus speed; tune after testing */
#define GHOST_TINT 0x80ffffffu         /* half-alpha white — semi-transparent  */

static void MoveGhost(void *p2)
{
    float *px = (float *)((char *)p2 + OFF_POS_X);
    float *py = (float *)((char *)p2 + OFF_POS_Y);
    if (*px <= GHOST_X_MIN) s_ghostDirX = 1;  else if (*px >= GHOST_X_MAX) s_ghostDirX = -1;
    if (*py <= GHOST_Y_TOP) s_ghostDirY = 1;  else if (*py >= GHOST_Y_BOT) s_ghostDirY = -1;
    *px += GHOST_SPEED * (float)s_ghostDirX;
    *py += GHOST_SPEED * (float)s_ghostDirY;
    /* semi-transparent ghost; the update rewrites the tint, so re-apply each frame */
    *(uint32_t *)((char *)p2 + OFF_TINT) = GHOST_TINT;
}

static void Spawn1Up(float *pos3)
{
    void *mgr = (void *)s_itemMgr;
    if (!mgr) { Log("1up spawn SKIPPED: item manager not seen yet"); return; }
    ADDR_ITEM_SPAWN(mgr, NULL, pos3, ITEM_TYPE_1UP, 0);
}

/* ---- ghost entry (phantom-spare aftermath) ----
 * The phantom spare (see HookedUpdate) routes a last-life death through ZUN's
 * NORMAL death path — partial power drop, no full-power items, no continue
 * reset, no game-over flag. When the phantom got consumed we take over here:
 * the player becomes a ghost (or, if the partner is already a ghost, the run
 * is genuinely over and WE raise the game-over flag). A guaranteed 1up drops
 * at the tracked death spot, and the revive bomb stock is pinned to the
 * alive-tracked value (overriding the vanilla respawn refill). */
static void EnterGhostP2(void)
{
    if (s_p1Ghost) {
        *ADDR_GAMEOVER = 1; s_runOver = 1;
        Log("P2 down while P1 is a ghost -> GAME OVER");
        return;
    }
    s_p2Ghost = 1;
    s_reviveFrames = 0;
    s_p2Bombs = s_p2AliveBombs;            /* death stock, not the respawn refill */
    Spawn1Up(s_p2AlivePos);
    Log("P2 last-life death -> 1up at (%.0f,%.0f); GHOST mode (bombs=%.0f power=%.0f)",
        s_p2AlivePos[0], s_p2AlivePos[1], s_p2Bombs, s_p2Power);
}

static void EnterGhostP1(void)
{
    if (s_p2Ghost) {
        *ADDR_GAMEOVER = 1; s_runOver = 1;
        Log("P1 down while P2 is a ghost -> GAME OVER");
        return;
    }
    s_p1Ghost = 1;
    s_reviveFrames = 0;
    {
        uint32_t res = *ADDR_RES_PTR;
        if (res) {
            *(float *)(res + RES_BOMBS) = s_p1AliveBombs;   /* death stock */
            ADDR_HUD_REFRESH(ADDR_SCORE_SINGLETON);
        }
    }
    Spawn1Up(s_p1AlivePos);
    Log("P1 last-life death -> 1up at (%.0f,%.0f); GHOST mode (bombs=%.0f)",
        s_p1AlivePos[0], s_p1AlivePos[1], s_p1AliveBombs);
}

/* Authentic graze feedback with NO stat effects: run the real graze-credit
 * leaf (spark + SFX), then put back the graze counters, the +200 score, and
 * the graze score-bonus accumulator it bumped. ZUN's own code writes these
 * fields directly (no accessor/checksum), so direct restore is equally safe. */
static void GrazeFeedback(void *player, float *pos)
{
    uint32_t res = *ADDR_RES_PTR;
    int g14 = 0, g18 = 0, sc = 0, acc = *ADDR_GRAZE_BONUS_ACC;
    if (res) {
        g14 = *(int *)(res + RES_GRAZE_CUR);
        g18 = *(int *)(res + RES_GRAZE_TOTAL);
        sc  = *(int *)(res + RES_SCORE);
    }
    ADDR_GRAZE_CREDIT(player, NULL, pos);
    if (res) {
        *(int *)(res + RES_GRAZE_CUR)   = g14;
        *(int *)(res + RES_GRAZE_TOTAL) = g18;
        *(int *)(res + RES_SCORE)       = sc;
    }
    *ADDR_GRAZE_BONUS_ACC = acc;
}

/* The survivor grazes the ghost for REVIVE_FRAMES consecutive frames (focus
 * state does NOT matter while channeling), then CONFIRMS with a focus RELEASE
 * (down->up edge) -> donates one life (free when they have no spare extends)
 * and the ghost resurrects next to them. Radius = hitbox-inside-sprite-ish
 * (user-tuned). While channeling, GrazeFeedback fires every few frames —
 * authentic graze SFX + spark, no stat changes. Symmetric: works for P1
 * reviving P2 AND P2 reviving P1. Runs while the shared struct holds P1's
 * values (after the P2 field-swap restore), so a P1 donation is a direct
 * write + checksum re-heal — the same proven pattern as the swap. */
#define REVIVE_RADIUS  24.0f
#define REVIVE_FRAMES  90
#define REVIVE_TICK     6          /* graze-feedback cadence while channeling */

static void ReviveByGraze(void *ghost, void *reviver, int reviverIsP1,
                          int focusRelease)
{
    unsigned char rst = *(unsigned char *)((char *)reviver + OFF_STATE);
    float dx = *(float *)((char *)reviver + OFF_POS_X)
             - *(float *)((char *)ghost + OFF_POS_X);
    float dy = *(float *)((char *)reviver + OFF_POS_Y)
             - *(float *)((char *)ghost + OFF_POS_Y);

    /* the reviver must be in a controllable state (play / respawn / border) */
    if ((rst == 0 || rst == 3 || rst == 4) &&
        dx > -REVIVE_RADIUS && dx < REVIVE_RADIUS &&
        dy > -REVIVE_RADIUS && dy < REVIVE_RADIUS) {
        if (s_reviveFrames % REVIVE_TICK == 0)      /* live feedback: real graze */
            GrazeFeedback(reviver, (float *)((char *)ghost + OFF_POS_X));
        s_reviveFrames++;
        if (s_reviveFrames >= REVIVE_FRAMES && focusRelease) {
            if (reviverIsP1) {
                uint32_t res = *ADDR_RES_PTR;
                if (res && *(float *)(res + RES_LIVES) >= 1.0f) {
                    *(float *)(res + RES_LIVES) -= 1.0f;    /* donate one */
                    ADDR_HUD_REFRESH(ADDR_SCORE_SINGLETON); /* re-heal checksum */
                    Log("revive: P1 donated a life (%.0f spare left)",
                        *(float *)(res + RES_LIVES));
                } else {
                    Log("revive: P1 has no spare extends -> free revive");
                }
                ReviveP2();
            } else {
                if (s_p2Lives >= 1.0f) {
                    s_p2Lives -= 1.0f;
                    Log("revive: P2 donated a life (%.0f spare left)", s_p2Lives);
                } else {
                    Log("revive: P2 has no spare extends -> free revive");
                }
                ReviveP1();
            }
            s_reviveFrames = 0;
        }
    } else {
        s_reviveFrames = 0;
    }
}

/* ---- life sharing between two LIVE players (EoSD-mod mechanic) ----
 * Both players graze each other (within REVIVE_RADIUS) for SHARE_FRAMES
 * consecutive frames with NEITHER shooting; then the DONOR confirms with one
 * focus release -> the donor loses a life and a 1up item pops out slightly
 * ABOVE the donor (spawn offset + the item's built-in upward pop), so the
 * partner can catch it instead of the donor instantly re-eating it. Pickup
 * stays universal on purpose: if the donor does re-eat it, the extend credit
 * refunds the donated life — net zero, nothing lost. */
#define SHARE_FRAMES   90
#define SHARE_POP_UP   48.0f       /* spawn this far above the donor          */
static int s_shareFrames = 0;

static void LifeShare(void *p2, uint16_t p1in, uint16_t p2in,
                      int p1FocusRelease, int p2FocusRelease)
{
    unsigned char p1st = *(unsigned char *)((char *)ADDR_PLAYER_BASE + OFF_STATE);
    unsigned char p2st = *(unsigned char *)((char *)p2 + OFF_STATE);
    float dx = *(float *)((char *)ADDR_PLAYER_BASE + OFF_POS_X)
             - *(float *)((char *)p2 + OFF_POS_X);
    float dy = *(float *)((char *)ADDR_PLAYER_BASE + OFF_POS_Y)
             - *(float *)((char *)p2 + OFF_POS_Y);

    if ((p1st == 0 || p1st == 3 || p1st == 4) &&
        (p2st == 0 || p2st == 3 || p2st == 4) &&
        !(p1in & IN_SHOOT) && !(p2in & IN_SHOOT) &&
        dx > -REVIVE_RADIUS && dx < REVIVE_RADIUS &&
        dy > -REVIVE_RADIUS && dy < REVIVE_RADIUS) {
        if (s_shareFrames % REVIVE_TICK == 0)
            GrazeFeedback((void *)ADDR_PLAYER_BASE, (float *)((char *)p2 + OFF_POS_X));
        s_shareFrames++;
        if (s_shareFrames >= SHARE_FRAMES && (p1FocusRelease || p2FocusRelease)) {
            float pos[3];
            if (p1FocusRelease) {               /* P1 donates */
                uint32_t res = *ADDR_RES_PTR;
                if (!res || *(float *)(res + RES_LIVES) < 1.0f) {
                    Log("life share: P1 has no spare life to donate"); return;
                }
                *(float *)(res + RES_LIVES) -= 1.0f;
                ADDR_HUD_REFRESH(ADDR_SCORE_SINGLETON);
                memcpy(pos, (char *)ADDR_PLAYER_BASE + OFF_POS_X, sizeof(pos));
                pos[1] -= SHARE_POP_UP;
                Spawn1Up(pos);
                Log("life share: P1 donated a life -> 1up at (%.0f,%.0f)", pos[0], pos[1]);
            } else {                            /* P2 donates */
                if (s_p2Lives < 1.0f) {
                    Log("life share: P2 has no spare life to donate"); return;
                }
                s_p2Lives -= 1.0f;
                memcpy(pos, (char *)p2 + OFF_POS_X, sizeof(pos));
                pos[1] -= SHARE_POP_UP;
                Spawn1Up(pos);
                Log("life share: P2 donated a life -> 1up at (%.0f,%.0f)", pos[0], pos[1]);
            }
            s_shareFrames = 0;
        }
    } else {
        s_shareFrames = 0;
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
    int f3  = (GetAsyncKeyState(VK_F3)  & 0x8000) != 0;
    int f2  = (GetAsyncKeyState(VK_F2)  & 0x8000) != 0;
    if (f9  && !s_prevF9)  { Log("F9 edge detected");  SpawnP2(); }
    if (f3  && !s_prevF3) {
        int cur = (s_p2Sel < 0) ? *ADDR_SEL_ID : s_p2Sel;
        s_p2Sel = cur ^ 1;                  /* A <-> B within current character */
        Log("F3: P2 shot type -> %s", ADDR_SEL_NAMES[s_p2Sel]);
        if (s_p2) ApplyP2Selection((void *)s_p2);   /* live re-apply is safe */
    }
    if (f2  && !s_prevF2) {
        int cur = (s_p2Sel < 0) ? *ADDR_SEL_ID : s_p2Sel;
        int type = cur & 1;
        int ch = (cur / 2 + 1) % 3;         /* cycle char Reimu->Marisa->Sakuya */
        s_allowDiffChar = 1;
        s_p2Sel = ch * 2 + type;
        Log("F2: P2 character -> %s", ADDR_SEL_NAMES[s_p2Sel]);
        if (s_p2) ApplyP2Selection((void *)s_p2);
    }
    if (f10 && !s_prevF10) { Log("F10 edge detected"); DespawnP2(); }
    if (f8  && !s_prevF8)  { s_p2Killable = !s_p2Killable; Log("P2 killable %s", s_p2Killable ? "ON" : "OFF"); }
    if (f7  && !s_prevF7)  { s_shotXfer = !s_shotXfer; Log("shot xfer %s", s_shotXfer ? "ON" : "OFF"); }
    if (f6  && !s_prevF6)  { s_p2SepRes = !s_p2SepRes; Log("P2 separate-resources %s", s_p2SepRes ? "ON" : "OFF"); }
    if (f5  && !s_prevF5)  { s_bossHpScale = !s_bossHpScale; Log("boss/enemy HP scaling %s", s_bossHpScale ? "ON" : "OFF"); }
    if (f4  && !s_prevF4)  { s_p2TeamBorder = !s_p2TeamBorder; Log("P2 team-border %s", s_p2TeamBorder ? "ON" : "OFF"); }
    if (f11 && !s_prevF11) {
        Log("F11 edge detected");
        if (s_p2Ghost) ReviveP2(); else if (s_p1Ghost) ReviveP1();
    }
    s_prevF9  = f9;
    s_prevF10 = f10;
    s_prevF8  = f8;
    s_prevF7  = f7;
    s_prevF6  = f6;
    s_prevF5  = f5;
    s_prevF4  = f4;
    s_prevF11 = f11;
    s_prevF3  = f3;
    s_prevF2  = f2;
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
    int isP1 = ((uint32_t)self == ADDR_PLAYER_BASE);
    int r = (isP1 && s_p1Ghost) ? 0                       /* ghost P1: untouchable */
          : s_origCollideBullet(self, edx, pos, size);    /* P1 (unchanged) */
    void *p2 = (void *)s_p2;
    if (s_p2Killable && p2 && isP1 && !s_p2Ghost && !P2CollisionSkipped(p2))
        s_origCollideBullet(p2, edx, pos, size);          /* P2 — param-relative */
    return r;
}

static int __fastcall HookedCollideLaser(void *self, void *edx,
                                         void *a, void *b, void *c, void *d, int e)
{
    int isP1 = ((uint32_t)self == ADDR_PLAYER_BASE);
    int r = (isP1 && s_p1Ghost) ? 0
          : s_origCollideLaser(self, edx, a, b, c, d, e); /* P1 (unchanged) */
    void *p2 = (void *)s_p2;
    if (s_p2Killable && p2 && isP1 && !s_p2Ghost && !P2CollisionSkipped(p2))
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
    int isP1 = ((uint32_t)self == ADDR_PLAYER_BASE);
    int r = (isP1 && s_p1Ghost) ? 0                       /* ghost P1: no grazing */
          : s_origGraze(self, edx, pos, size);            /* P1 (unchanged) */
    void *p2 = (void *)s_p2;
    if (p2 && isP1 && !s_p2Ghost && !P2CollisionSkipped(p2)) {
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

    int isP1 = ((uint32_t)self == ADDR_PLAYER_BASE);
    int r = (isP1 && s_p1Ghost) ? 0                       /* ghost P1: can't collect */
          : s_origCollectOverlap(self, edx, pos, size);   /* P1 (unchanged) */
    void *p2 = (void *)s_p2;
    if (isP1 && !r && p2 && !s_p2Ghost) {
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

/* ---- boss/enemy HP scaling detour (damage-side) ----
 * Run the original (side effects — shot consumption, hit sparks — happen once,
 * unchanged), then divide ONLY the returned damage by the active player count,
 * flooring a positive result at 1 so weak shots still register. Catches every
 * enemy including the midboss/boss whose HP never passes the ECL set-life path
 * (proven by the 2026-06-12 eclhp log). Arms only while P2 is live. */
static int __fastcall HookedDamage(void *self, void *edx,
                                   float *pos, float *size, int *out_flag)
{
    int r = s_origDamage(self, edx, pos, size, out_flag);
    void *p2 = (void *)s_p2;

    /* P2 SHOT + BOMB DAMAGE: the per-enemy sweep is hardwired to ECX = the
     * static P1 (bisect-proven), so re-invoke param-relative for a live P2 —
     * its own array's shots, its lasers, and its bomb all get tested. With the
     * shot transfer OFF (the default now), each shot lives in exactly one
     * array, so nothing is double-counted. */
    if (p2 && (uint32_t)self == ADDR_PLAYER_BASE && !s_p2Ghost) {
        int f2 = 0;
        int r2;
        SwapSelGlobals(1);
        r2 = s_origDamage(p2, edx, pos, size, &f2);
        SwapSelGlobals(0);
        if (r2 > 0) r += r2;
        if (f2 && out_flag && !*out_flag) *out_flag = f2;
    }

    if (s_bossHpScale && s_p2 && r > 0) {
        r /= 2;                              /* player count (3P: divide by 3) */
        if (r == 0) r = 1;
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
/* STAGE START: the logic-frame counter went BACKWARDS = a fresh stage/game
 * task. The P2 clone's internal pointers reference the PREVIOUS stage's
 * assets, which the load just recycled (proven crash: stale P2 locked in
 * place, then crashed on shoot in stage 2) — rebuild the co-op session:
 * despawn the stale clone, re-arm auto-spawn (P2 returns ~3s in, cloned from
 * the FRESH P1). Resources carry across stages; after a game over the next
 * run reseeds from P1. A ghost is revived by the transition (still 0 spares). */
static int __fastcall HookedFrameTask(int *self)
{
    int r = s_origFrameTask(self);
    int f = *self;
    if (f < s_lastLogicFrame && (s_p2 || s_p1Ghost || s_runOver)) {
        if (s_runOver) {
            Log("frame counter reset after game over -> full co-op reset");
            s_p2Carry = 0;              /* fresh run: reseed P2 from P1 */
        } else {
            Log("frame counter reset (stage start) -> P2 rebuild (resources carried)");
            s_p2Carry = (s_p2 != NULL);
        }
        s_runOver = 0;
        s_p1Ghost = 0;
        s_p2Ghost = 0;
        s_reviveFrames = 0;
        s_shareFrames = 0;
        *(uint32_t *)((char *)ADDR_PLAYER_BASE + OFF_TINT) = 0xffffffffu;
        DespawnP2();                /* the old clone references the previous stage */
        s_autoSpawned = 0;          /* re-arm auto-spawn */
        s_readyFrames = 0;
    }
    s_lastLogicFrame = f;
    return r;
}

static int __fastcall HookedUpdate(void *self)
{
    int isP1 = ((uint32_t)self == ADDR_PLAYER_BASE);
    int p1Fake = 0;
    uint16_t p1GhostSavedIn = 0;

    /* PHANTOM SPARE (P1): while co-op is active, ZUN's death commit must never
     * see 0 lives — the lives==0 path is game-over + full-power items + the
     * continue-style reset. With a phantom spare swapped in, a last-life death
     * runs the NORMAL death (partial power drop, vanilla respawn); afterwards
     * we detect the consumed phantom and overlay ghost mode ourselves. */
    if (isP1 && s_p2 && !s_runOver) {
        if (!s_p1Ghost) {
            uint32_t res = *ADDR_RES_PTR;
            if (res && *(float *)(res + RES_LIVES) < 1.0f) {
                *(float *)(res + RES_LIVES) = 1.0f;
                ADDR_HUD_REFRESH(ADDR_SCORE_SINGLETON);
                p1Fake = 1;
            }
        } else {
            p1GhostSavedIn = *ADDR_INPUT_GAMEPLAY;   /* ghost P1: no input at all */
            *ADDR_INPUT_GAMEPLAY = 0;
        }
    }

    int r = s_origUpdate(self);             /* P1 (or whatever ctx the task holds) */

    /* act only off the real P1 object, never re-entrantly off P2 */
    if (isP1) {
        if (s_p1Ghost) {
            *ADDR_INPUT_GAMEPLAY = p1GhostSavedIn;
            MoveGhost((void *)ADDR_PLAYER_BASE);
        }
        if (p1Fake) {
            uint32_t res = *ADDR_RES_PTR;
            float lv = res ? *(float *)(res + RES_LIVES) : 1.0f;
            if (lv < 1.0f) {
                EnterGhostP1();             /* phantom consumed -> last-life death */
            } else if (res) {
                *(float *)(res + RES_LIVES) = lv - 1.0f;    /* take the phantom back */
                ADDR_HUD_REFRESH(ADDR_SCORE_SINGLETON);
            }
        }
        /* track P1's last ALIVE pos + bomb stock (1up drop spot / revive bombs) */
        if (!s_p1Ghost) {
            unsigned char st = *(unsigned char *)((char *)ADDR_PLAYER_BASE + OFF_STATE);
            if (st == 0 || st == 4) {
                uint32_t res = *ADDR_RES_PTR;
                memcpy(s_p1AlivePos, (char *)ADDR_PLAYER_BASE + OFF_POS_X,
                       sizeof(s_p1AlivePos));
                if (res) s_p1AliveBombs = *(float *)(res + RES_BOMBS);
            }
        }

        PollHotkeys();

        /* auto-spawn shortly after P1 finishes the stage fly-in (state 0) */
        if (!s_autoSpawned && !s_p2) {
            if (P1Ready() &&
                *(unsigned char *)((char *)ADDR_PLAYER_BASE + OFF_STATE) == 0) {
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
            if (s_p2Ghost) p2in = 0;    /* ghost: no input at all — MoveGhost drives it */

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
            int p2Fake = 0;
            uint32_t res = *ADDR_RES_PTR;
            unsigned char goBefore = *ADDR_GAMEOVER;
            if (s_p2SepRes && res) {
                rl = (float *)(res + RES_LIVES);
                rb = (float *)(res + RES_BOMBS); rp = (float *)(res + RES_POWER);
                saveL = *rl; saveB = *rb; saveP = *rp;
                ckBefore = *(volatile uint32_t *)(res + RES_CHECKSUM);
                /* PHANTOM SPARE (P2): same trick as P1's — swap in 1 when P2 has
                 * no spares so a last-life death runs ZUN's NORMAL death path. */
                p2Fake = (!s_p2Ghost && !s_runOver && s_p2Lives < 1.0f);
                *rl = p2Fake ? 1.0f : s_p2Lives;
                *rb = s_p2Bombs; *rp = s_p2Power;
            }

            /* Drive P2's ringless shadow border to track P1's (enter/sync/retire) BEFORE
             * P2's update, so P2 updates in the correct border state. */
            UpdateTeamBorder(p2);

            s_inP2Update = 1;               /* effect-spawn detour: redirect slot */
            SwapSelGlobals(1);              /* char/type-gated branches see P2 */
            SwapAnm(1);                     /* 0x400-range tables -> P2's char */
            s_origUpdate(p2);               /* trampoline → no detour re-entry */
            SwapAnm(0);
            SwapSelGlobals(0);
            s_inP2Update = 0;

            if (s_p2SepRes && rb) {
                s_p2Lives = *rl; s_p2Bombs = *rb; s_p2Power = *rp;  /* capture P2's */
                /* resolve the phantom: consumed -> last-life death -> ghost;
                 * untouched -> take it back (real spares stay 0) */
                if (p2Fake) {
                    if (s_p2Lives < 1.0f) { s_p2Lives = 0.f; EnterGhostP2(); }
                    else                    s_p2Lives -= 1.0f;
                }
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

            /* With the phantom spare, ZUN's lives==0 game-over can no longer fire
             * during P2's update — cancel anything unexpected (but never OUR
             * deliberate both-ghosts game over). */
            if (!goBefore && *ADDR_GAMEOVER && !s_runOver) {
                *ADDR_GAMEOVER = 0;
                Log("unexpected game-over flag during P2 update -> cancelled");
            }

            *ADDR_INPUT_GAMEPLAY = saved;   /* restore P1's input immediately */

            /* log P2 death-FSM transitions (0 play,2 dying,3 respawn,1 enter,4 border) */
            {
                unsigned char st = *(unsigned char *)((char *)p2 + OFF_STATE);
                if (st != s_p2PrevState) {
                    Log("P2 state %d -> %d", s_p2PrevState, st);
                    s_p2PrevState = st;
                }
                /* track the last ALIVE position + bomb stock (1up drop / revive) */
                if (!s_p2Ghost && (st == 0 || st == 4)) {
                    memcpy(s_p2AlivePos, (char *)p2 + OFF_POS_X, sizeof(s_p2AlivePos));
                    s_p2AliveBombs = s_p2Bombs;
                }
            }

            /* P2 focus ring: the effect-spawn detour redirected P2's vanilla
             * spawn into slot 406, so ZUN's machine owns the lifecycle via
             * p2+0x9d8. We only counter the updater's per-frame P1-snap. */
            {
                uint32_t fx = *(uint32_t *)((char *)p2 + OFF_PLAYER_FX);
                s_p2FocusFx = (fx && *(unsigned char *)(fx + OFF_FX_TYPE) == FX_TYPE_FOCUS)
                            ? (void *)fx : NULL;
                if (s_p2FocusFx)
                    memcpy((char *)s_p2FocusFx + OFF_FX_POS,
                           (char *)p2 + OFF_POS_X, 12);
            }

            /* ghost drive + graze resurrection / live life-sharing: the shared
             * struct holds P1's values here (restored above), so a life
             * donation can write it directly. All mechanics confirm on a
             * focus-release edge (P1 = the restored gameplay word; P2 = the
             * p2in word fed this frame, zeroed while ghost). At most one player
             * can be a ghost — a second down while one is a ghost is game over. */
            if (!s_runOver) {
                uint16_t p1in = *ADDR_INPUT_GAMEPLAY;
                int p1Rel = s_p1PrevFocus && !(p1in & IN_FOCUS);
                int p2Rel = s_p2PrevFocus && !(p2in & IN_FOCUS);
                if (s_p2Ghost) {
                    MoveGhost(p2);
                    ReviveByGraze(p2, (void *)ADDR_PLAYER_BASE, 1, p1Rel);
                } else if (s_p1Ghost) {
                    /* MoveGhost(P1) already ran right after P1's update */
                    ReviveByGraze((void *)ADDR_PLAYER_BASE, p2, 0, p2Rel);
                } else {
                    LifeShare(p2, p1in, p2in, p1Rel, p2Rel);
                }
                s_p1PrevFocus = (p1in & IN_FOCUS) != 0;
                s_p2PrevFocus = (p2in & IN_FOCUS) != 0;
            }

            if (s_shotXfer && !s_p2Ghost) TransferP2Shots(p2);  /* P2 shots -> P1 array */
        }
    }
    return r;
}

/* P2 HUD: queue text on ZUN's ascii manager once per frame (rendered by its
 * own draw task in the sidebar, where the rest of the HUD lives). Shows P2's
 * separate resources, ghost status, and the revive/share channel progress. */
static void DrawCoopHud(void *p2)
{
    float pos[3];
    pos[0] = 448.f; pos[2] = 0.f;

    pos[1] = 296.f;
    if (s_p2Ghost)
        ADDR_ASCII_PRINT(ADDR_ASCII_MGR, pos, "P2 GHOST");
    else {
        int psel = (s_p2Sel >= 0 ? s_p2Sel : *ADDR_SEL_ID);
        char ch = "RMS"[(psel / 2) % 3];          /* Reimu/Marisa/Sakuya       */
        ADDR_ASCII_PRINT(ADDR_ASCII_MGR, pos, "P2%c%c L%d B%d P%d",
                         ch, (psel & 1) ? 'B' : 'A',
                         (int)s_p2Lives, (int)s_p2Bombs, (int)s_p2Power);
    }
    if (s_p1Ghost) {
        pos[1] = 312.f;
        ADDR_ASCII_PRINT(ADDR_ASCII_MGR, pos, "P1 GHOST");
    }
    if (s_reviveFrames > 0) {
        pos[1] = 328.f;
        ADDR_ASCII_PRINT(ADDR_ASCII_MGR, pos, "REVIVE %d/%d",
                         s_reviveFrames > REVIVE_FRAMES ? REVIVE_FRAMES : s_reviveFrames,
                         REVIVE_FRAMES);
    } else if (s_shareFrames > 0) {
        pos[1] = 328.f;
        ADDR_ASCII_PRINT(ADDR_ASCII_MGR, pos, "SHARE %d/%d",
                         s_shareFrames > SHARE_FRAMES ? SHARE_FRAMES : s_shareFrames,
                         SHARE_FRAMES);
    }
    (void)p2;
}

static int __fastcall HookedDraw(void *self)
{
    int r = s_origDraw(self);               /* P1 draw */

    if ((uint32_t)self == ADDR_PLAYER_BASE) {
        void *p2 = (void *)s_p2;
        if (p2) {
            /* re-pin the P2 focus ring before anything draws it — the effect
             * updater snaps it to P1 each tick and the update order between the
             * player and effect tasks is unknown; the draw chain runs last */
            if (s_p2FocusFx)
                memcpy((char *)s_p2FocusFx + OFF_FX_POS,
                       (char *)p2 + OFF_POS_X, 12);
            /* feed P2 this frame's homing + SakuyaA aim targets (see the
             * OFF_HOMING_TGT note) — the consumers run param-relative but the
             * enemy update only ever fills static P1's block */
            memcpy((char *)p2 + OFF_HOMING_TGT,
                   (char *)ADDR_PLAYER_BASE + OFF_HOMING_TGT, HOMING_TGT_LEN);
            SwapSelGlobals(1);
            SwapAnm(1);                     /* P2's char sprites for its draw */
            s_origDraw(p2);
            SwapAnm(0);
            SwapSelGlobals(0);
            DrawCoopHud(p2);
        }
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
    if (MH_CreateHook(ADDR_DAMAGE,         (LPVOID)&HookedDamage,        (LPVOID*)&s_origDamage)        != MH_OK) return 0;
    if (MH_CreateHook(ADDR_EFFECT_SPAWN,   (LPVOID)&HookedEffectSpawn,   (LPVOID*)&s_origEffectSpawn)   != MH_OK) return 0;
    if (MH_CreateHook(ADDR_FRAME_TASK,     (LPVOID)&HookedFrameTask,     (LPVOID*)&s_origFrameTask)     != MH_OK) return 0;
    if (MH_CreateHook(ADDR_COLLECT_OVERLAP,(LPVOID)&HookedCollectOverlap,(LPVOID*)&s_origCollectOverlap) != MH_OK) return 0;
    if (MH_CreateHook(ADDR_ITEM_LOOP,      (LPVOID)&HookedItemLoop,      (LPVOID*)&s_origItemLoop)      != MH_OK) return 0;
    if (MH_CreateHook(ADDR_BORDER_BREAK,   (LPVOID)&HookedBorderBreak,   (LPVOID*)&s_origBorderBreak)   != MH_OK) return 0;
    if (MH_EnableHook(ADDR_PLAYER_UPDATE)  != MH_OK) return 0;
    if (MH_EnableHook(ADDR_PLAYER_DRAW)    != MH_OK) return 0;
    if (MH_EnableHook(ADDR_COLLIDE_BULLET) != MH_OK) return 0;
    if (MH_EnableHook(ADDR_COLLIDE_LASER)  != MH_OK) return 0;
    if (MH_EnableHook(ADDR_COLLIDE_GRAZE)  != MH_OK) return 0;
    if (MH_EnableHook(ADDR_DAMAGE)         != MH_OK) return 0;
    if (MH_EnableHook(ADDR_EFFECT_SPAWN)   != MH_OK) return 0;
    if (MH_EnableHook(ADDR_FRAME_TASK)     != MH_OK) return 0;
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
                "damage @0x43d9e0, collect-overlap @0x43e4e0, item-loop @0x432990, "
                "border-break @0x441bd0)");
        break;
    case DLL_PROCESS_DETACH:
        if (s_log) { Log("detach"); fclose(s_log); }
        MH_Uninitialize();
        break;
    }
    return TRUE;
}
