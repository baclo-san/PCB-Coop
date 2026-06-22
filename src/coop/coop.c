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
 *   survivor's lives; with NO spare extend the revive cannot fire (D1,
 *   user-confirmed 2026-06-16 — no free revives).
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
 *   F12 = toggle P2 HUD style: sprite ICONS that mirror P1's life/bomb rows
 *         (default, handoff §8a) <-> the legacy ascii "P2xx Ln Bn Pn" text line.
 *
 * NETPLAY (coop.ini [net] enabled=1): P2's input comes from the WIRE instead of
 *   the local keyboard — the engine-agnostic netcode core is linked straight in
 *   (docs/th07_fork_a_integration.md §8). Detours FUN_00437c70 (per-frame input
 *   merge into g_InputMenu, owns the lockstep frame counter) + FUN_00442c60
 *   (shared RNG-seed force); P2 reads the merged high bits (UnpackP2). Default
 *   off => unchanged local-keyboard baseline. host=P1, guest=P2; each player
 *   picks its OWN character at the select screen (the two-pass FSM is driven by
 *   the de-merged per-player wire words). See coop.ini for role/peer/seed.
 * PROXIMITY FADE (coop.ini [coop] proximity_fade=1, default off): fade the other
 *   player out as the two overlap (asymmetric: host fades P2, guest fades P1).
 *
 * !!! ALL ADDRESSES are build-specific to th07.exe ver 1.00b
 * !!! SHA256 35467EAF8DC7FC85F024F16FB2037255F151CEFDA33CF4867BC9122AAA2E80CA
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <float.h>
#include "MinHook.h"

/* ---- build-specific addresses (Ghidra db: th07.exe.c ver 1.00b) ---- */
#define ADDR_PLAYER_BASE    0x004bdad8u            /* static player object      */
#define PLAYER_SIZE         0x000b7e78u            /* sizeof player object       */
#define ADDR_PLAYER_UPDATE  ((LPVOID)0x00441fb0)   /* __fastcall(ecx=player)     */
#define ADDR_PLAYER_DRAW    ((LPVOID)0x004420b0)   /* __fastcall(ecx=player)     */
#define ADDR_INPUT_GAMEPLAY ((volatile uint16_t*)0x004b9e50) /* g_InputGameplay  */
#define ADDR_REPLAY_MGR  ((void **)0x004b9e48)   /* -> replay manager (NULL until a game; §8ac trace) */

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

/* B4 — bomb bullet-clear for P2. PCB's bomb clears bullets via per-player CLEAR-REGION
 * slots at player+0x17dc (96 × 0x20: x,y,w,h,radius,…,type + timer@+0x18). The bullet
 * HIT test FUN_0043e260 calls FUN_0043e0a0(player, bulletPos) — a bullet inside any of
 * the player's regions returns "clear" (state 5). coop ALREADY re-invokes FUN_0043e260
 * for P2, so it reads P2+0x17dc. The regions are registered by FUN_004418b0 (circle) /
 * FUN_00441800 (box), __thiscall(ECX=player, pos*, …), called only from the per-char
 * bomb callbacks. The reported bug = P2's bomb registers nothing on P2+0x17dc (its
 * callback threads the P1 static base for ECX — the documented coop ECX pitfall). Fix:
 * detour both registrars and, while inside P2's update window, force ECX to P2. This is
 * safe-by-construction: +0x17dc is bomb-only and during P2's update the only legitimate
 * region owner is P2, so the redirect is a no-op if ECX was already P2 and the fix
 * otherwise. */
#define ADDR_ADD_CLEAR_CIRCLE ((LPVOID)0x004418b0) /* __thiscall(player,pos,r,col,?,type) */
#define ADDR_ADD_CLEAR_BOX    ((LPVOID)0x00441800) /* __thiscall(player,pos,w,h,type)      */
#define OFF_CLEAR_REGIONS 0x17dc

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
/* B4 clear-region registrars (__thiscall(player, pos*, …)). */
typedef void *(__fastcall *AddClearCircleFn_t)(void *self, void *edx, void *pos,
                                               uint32_t r, uint32_t col, uint32_t type);
typedef void *(__fastcall *AddClearBoxFn_t)(void *self, void *edx, void *pos,
                                            uint32_t w, uint32_t h, uint32_t type);

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
static AddClearCircleFn_t s_origAddClearCircle = NULL;  /* B4 */
static AddClearBoxFn_t    s_origAddClearBox    = NULL;  /* B4 */
static int s_p2BombClear = 1;          /* B4: P2 bomb clears bullets (default on) */
static int s_p2PrevBombing = 0;        /* B5 diag: P2 bombing-flag edge tracker    */
static int s_b5ZeroCount   = 0;        /* B5 diag: times B5 zeroed boss damage/bomb */
static int s_b5HitLogged   = 0;        /* B5 diag: one-shot @hit dump per bomb      */
static DamageFn_t        s_origDamage        = NULL;
static ItemLoopFn_t      s_origItemLoop       = NULL;

/* B5: the enemy-manager base (FUN_00420620's param_1/ECX). The boss spellcard flags live
 * at base+0x9545c8 (isActive) / base+0x9545dc (usedBomb) — param_1 is NOT 0 (the in-game
 * boss enemy logged at 0x009b3998), so we capture the live base each enemy-update tick. */
typedef int (__fastcall *EnemyUpdateFn_t)(void *self);
#define ADDR_ENEMY_UPDATE ((LPVOID)0x00420620)
static EnemyUpdateFn_t s_origEnemyUpdate = NULL;
static volatile uint32_t s_enemyMgr = 0;
/* Post-spell-capture refuel window. When P1 is full the boss substitutes POINT items
 * for the power drop it would give (confirmed in-game: capture reward = a type-1 burst
 * at p1pow=128, in the sc=0 gap after the spell ends). Co-op wants those as POWER when
 * P2 is below full, so HookedEnemyUpdate opens this countdown on each boss spell-end and
 * HookedItemSpawn converts type-1 -> type-0 while it's open and P2 isn't full. */
static int s_captureWin = 0;
#define CAPTURE_WIN_FRAMES 240   /* ~4s after a boss spell ends (covers the bonus + fountain) */

/* ---- aim & item-suction: target the NEARER player (coop gameplay) ----
 * FUN_00442370 is Player::AngleToPlayer(this=player, pos): the angle from world
 * point `pos` to the player's centre (player+0x930/+0x934, atan2). It is the ONE
 * choke point every "toward the player" direction flows through -- enemy aimed
 * shots (ECL angle-to-player 7825/8142; bullet-pattern fire 14298/14346; lasers;
 * 9591/14538) AND item-collection suction (FUN_00432990 @20399). We hook it and,
 * choosing the player nearer to `pos` (or P2 when P1 is a ghost), pass THAT
 * player as `this` to the original -- so shots and items track whoever was closer
 * at fire/suction time. No position/cache poking, no hot ECL-VM hook (the EoSD
 * clean decomp BulletManager::AngleProvokedPlayer confirmed this is the seam).
 * __thiscall (ECX=player, pos on stack), returns float10 = x87 long double. */
typedef long double (__fastcall *AngleToPlayerFn_t)(void *player, void *edx, float *pos);
#define ADDR_ANGLE_TO_PLAYER ((AngleToPlayerFn_t)0x00442370)
static AngleToPlayerFn_t s_origAngleToPlayer = NULL;
static int s_coopAim = 1;               /* aim/suction-at-nearer enabled (default on) */

static volatile void *s_p2   = NULL;   /* P2 object base, NULL until spawned     */
static int   s_p2Killable = 1;         /* on by default; F8 toggles              */
static unsigned char s_p2PrevState = 0;/* for death-transition logging           */
static int   s_prevF9 = 0, s_prevF10 = 0, s_prevF8 = 0, s_prevF7 = 0, s_prevF6 = 0, s_prevF11 = 0, s_prevF5 = 0, s_prevF4 = 0, s_prevF3 = 0, s_prevF2 = 0, s_prevF12 = 0;
/* P2 HUD style: 1 = sprite ICONS that mirror P1's life/bomb rows (handoff §8a);
 * 0 = the legacy ascii "P2xx Ln Bn Pn" line. F12 toggles (text is the fallback).
 * Icons now draw from CAPTURED copies of ZUN's life/bomb icon objects (snapshot
 * at HUD-draw entry, see CaptureIconTemplates) instead of poking ZUN's live
 * structs -- the latter crashed non-deterministically (2026-06-14). */
static int   s_p2IconHud = 1;

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
#define OFF_FX_COLOR      0x1b8    /* u32 ARGB tint (param_6 of FUN_0041c610@26415)*/
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
static int s_lastScore = 0;            /* prev frame's team score; a DROP at a frame
                                          reset = retry/fresh start (vs stage advance,
                                          which carries the score). Phantom-spare logic
                                          never touches score, so it's a clean signal. */

/* ZUN's on-screen text: FUN_00402060(ascii_mgr, float pos[3], fmt, ...) —
 * vsprintf into a local buffer, then queue on the ascii manager (static at
 * 0x0134ce18); the manager's own draw task renders the queue each frame
 * (16px line height, sidebar coords — the HUD "%d/%d" point display uses
 * (496,176), PCBdecomp.c:17163). Queueing is render-state-independent; call
 * once per frame. Plain __cdecl varargs. */
typedef void (__cdecl *AsciiPrintFn_t)(void *mgr, float *pos, const char *fmt, ...);
#define ADDR_ASCII_PRINT ((AsciiPrintFn_t)0x00402060)
#define ADDR_ASCII_MGR   ((void *)0x0134ce18)

/* ---- P1-style ICON HUD for P2 (handoff §8a) ----
 * The in-game sidebar/score draw FUN_0042b603 (__fastcall, ECX = score-manager
 * singleton 0x626270) paints the whole right-hand HUD onto a persistent surface:
 * the dark background tiles redraw only on a "full-dirty" condition, and each
 * value section (lives/bombs/power/...) redraws only when its 2-bit dirty field
 * in *(singleton+4) is set. Lives are drawn as a ROW OF ICONS (one front.anm
 * sprite per life) and bombs likewise; power + point-items are ascii numbers.
 *   Life-icon sprite object: *(singleton+8) [= the score data struct, *0x626278]
 *     + 0x14ac. Bomb-icon object: + 0x16f8. Each is a baked anm sprite already
 *     bound to the correct front.anm id; the draw just writes screen X/Y/scale at
 *     +0x1c8 / +0x1cc / +0x1d0 and calls the sprite blit FUN_0044f770(obj).
 *   ZUN's P1 rows: X origin 496, +16px per icon, lives Y=96, bombs Y=112,
 *     icon scale 0x3eeb851f; power number at (496,160); point-items "%d/%d" at
 *     (496,176). (PCBdecomp.c FUN_0042b603 @16832; sprite blit FUN_0044f770 @33506.)
 * We hook FUN_0042b603, FORCE the full sidebar redraw each frame (set the redraw
 * flag at struct+0x1d70, one of the OR-terms of the 16943 condition) so a P2
 * count DROP can't leave a stale icon on the persistent surface, let the original
 * paint P1's HUD, then append P2's life/bomb icon quads + power number BELOW the
 * point-item line. FUN_0044f770 self-validates the object (no-ops if unbound), so
 * the calls are crash-safe. F12 falls back to the legacy ascii text line. */
#define ADDR_HUD_DRAW     ((LPVOID)0x0042b603)   /* __fastcall(ecx = score singleton) */
#define ADDR_SPRITE_BLIT  ((SpriteBlitFn_t)0x0044f770)  /* AnmManager::Draw, __thiscall(ecx=mgr, obj) */
#define ADDR_SPRITE_FLUSH ((SpriteFlushFn_t)0x0044f5c0) /* __fastcall(anmMgr) draw+reset batch */
#define ADDR_SPRITE_MGR   ((volatile uint32_t *)0x004b9e44) /* -> anm sprite-batch mgr base   */
/* Sidebar redraw-request counter (DAT_00575ab4): ZUN sets it to 2 whenever a HUD
 * value changes and decrements it each frame; while it's nonzero FUN_0042b603
 * fully redraws the cached sidebar panel (bg tiles + every element). We keep it
 * >=2 every frame P2 is live so the panel is cleared+repainted each frame -- the
 * fix for P2's own per-frame text (power, GHOST/REVIVE) smearing on a stale panel.
 * It's a plain global ZUN writes constantly, so setting it here is safe. */
#define ADDR_HUD_REDRAW_REQ ((volatile int *)0x00575ab4)
#define HUD_OFF_LIFE_ICON 0x14ac   /* *(hud+8) off: baked life-icon sprite object */
#define HUD_OFF_BOMB_ICON 0x16f8   /* *(hud+8) off: baked bomb-icon sprite object */
#define HUD_OFF_REDRAW    0x1d70   /* *(hud+8) off: nonzero => full sidebar redraw */
#define HUD_SPR_X         0x1c8    /* sprite-object off: screen X (float)              */
#define HUD_SPR_Y         0x1cc    /* sprite-object off: screen Y (float)              */
#define HUD_SPR_Z         0x1d0    /* sprite-object off: scale/Z (float)               */
#define HUD_ICON_SCALE    0x3eeb851fu  /* ZUN's life/bomb icon scale (~0.46)          */
#define HUD_ICON_X        496.f    /* same column as P1's rows                         */
#define HUD_ICON_STEP     16.f     /* per-icon X step                                 */
#define HUD_P2_LIVES_Y    192.f    /* just below P1's point-item line (Y=176)          */
#define HUD_P2_BOMBS_Y    208.f    /* P1's 16px row rhythm continued                   */
#define HUD_P2_POWER_Y    224.f
#define HUD_P2_LABEL_X    472.f    /* "2P" marker left of the icon row                */
#define HUD_ICON_MAX      9        /* clamp icon count so a row can't overrun         */
/* FUN_0044f770 is AnmManager::Draw(AnmVm*) -- a __thiscall method: ecx = the anm
 * manager (*0x4b9e44), the sprite object on the stack. ZUN sets ecx every call;
 * we MUST too (the appender derefs the manager via that ecx, e.g. mgr+0x2e4cc).
 * GCC has no __thiscall keyword for C, so we emulate it with __fastcall: arg1->ecx
 * (manager), arg2->edx (dummy, unused by the callee), arg3->stack (object). Stack
 * cleanup matches (callee pops the single stack arg in both conventions). */
typedef int  (__fastcall *SpriteBlitFn_t)(void *mgr, int edx, void *spriteObj);
typedef void (__fastcall *SpriteFlushFn_t)(void *anmMgr);
typedef void (__fastcall *HudDrawFn_t)(void *singleton);
static HudDrawFn_t s_origHudDraw = NULL;
#define SPR_OBJ_SIZE     0x24c                  /* HUD sprite-object stride (0x14ac->0x16f8)*/
#define SPR_OFF_FLAGS    0x1c0                  /* sprite-obj: draw-enable bits (b0|b1) + flip*/
#define SPR_OFF_ANM      0x1e4                  /* sprite-obj: bound anm sprite-entry ptr  */
/* We draw P2's life/bomb rows by re-blitting ZUN's OWN live icon objects (at
 * *(hud+8)+0x14ac / +0x16f8) -- the exact objects ZUN blits for P1 every frame --
 * after temporarily writing P2's row position and restoring it. Blitting a COPY
 * crashed inside the renderer's appender (FUN_0044efb0 reaches object-relative
 * state our standalone buffer can't satisfy); the live object always works. We do
 * this AFTER s_origHudDraw, so P1's HUD is already painted and ZUN won't read the
 * object again until next frame (when it rewrites the position anyway). */
static int s_lifeReady = 0, s_bombReady = 0;   /* "drew P2's row at least once" trackers */

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

/* ── B2: power-item → cherry conversion gated on BOTH players full ──────────────
 * Vanilla PCB turns dropped power items into cherry (point) items as soon as P1 is
 * at full power — which in co-op starves P2's only power source. We want the
 * conversion to wait until BOTH players are full.
 *
 * The decision lives inside the spawner FUN_004326f0. An EARLIER reading of the
 * decomp (Ghidra modelled the round as `FUN_0048b8a0()` reading in_ST0) concluded the
 * caller pushes P1's power on the x87 ST0, so the convert couldn't be wrapped by a C
 * detour and was attacked with a NAKED ST0 trampoline. That was WRONG, and is exactly
 * why B2 "never worked regardless of config": the disassembly shows the function loads
 * P1's power FRESH FROM MEMORY and ignores the caller's ST0 —
 *     432710  mov eax, [0x626278]          ; res base  == *ADDR_RES_PTR
 *     432715  fld dword ptr [eax+0x7c]     ; ST0 = P1 power  (RES_POWER)
 *     432718  call 0x48b8a0                ; round(power)
 *     43271d  cmp eax, 0x80 ; (>=128) && type∈{0,2} ⇒ type = 7 (cherry)
 * So the old detour's fstp/fldz mutated a dead ST0 that 432715 immediately overwrote.
 *
 * Correct, FPU-free fix: a normal thiscall detour. When we want to keep the drop as a
 * power item (g_b2Suppress: P2 live and below full) and the item is a convertible type
 * (0/2), temporarily write P1's power to 0.0f around the original call, so the engine's
 * own `round(*(res+0x7c)) >= 0x80` test fails and it leaves the type unchanged; then
 * restore. 0.0f is all-zero bits ⇒ the save/zero/restore is a plain integer memory copy
 * (no FPU, no RNG, deterministic), and the anti-tamper checksum (res+0xb0) is untouched
 * because power is restored before any accessor runs. The convert test is the spawner's
 * SOLE power read (432715 verified), so nothing else is disturbed.
 *
 * g_b2Suppress is computed in C, FP-safely, once per frame:
 *   P2 live && P2 power < full  (i.e. P1 full + P2 not full ⇒ keep it a power item).
 * When P2 is also full we leave power alone, so vanilla converts (both full). Only the
 * convertible types (0,2) are touched; every other spawn — including coop's own 1up
 * drops (type 5) — is a clean passthrough. Gated behind coop.ini [coop] cherry_both_full
 * (default ON since the 2026-06-17 x87 desync fix; set it to 0 to restore vanilla).
 * thiscall: ECX=mgr (this), then pos*, type, mode on the stack. */
static void Log(const char *fmt, ...);  /* defined below; used by the early item-spawn diag */
static int  SpellcardActive(void);      /* defined below; boss-spell flag for the spawn diag */
unsigned char g_b2Suppress      = 0;    /* read by the detour: keep this spawn as power */
void         *g_b2OrigItemSpawn = NULL; /* MinHook trampoline for FUN_004326f0        */
volatile uint32_t g_b2Calls      = 0;   /* diag: spawner calls seen by the hook        */
volatile uint32_t g_b2Suppressed = 0;   /* diag: conversions suppressed (kept as power)*/
static int s_b2LoggedFire = 0;          /* one-shot: logged the first real suppression */
static int s_b2RefuelLogged = 0;        /* one-shot: logged the first post-capture point->power */
static int s_b2DiagN      = 0;          /* diag: count of logged convertible/converted spawns */

/* (ItemSpawnFn_t / FUN_004326f0 typedef declared above with the spawner addr.) */
static int __fastcall HookedItemSpawn(void *self, void *edx, float *pos, int type, int mode)
{
    g_b2Calls++;
    /* DIAG (cherry/power): user-confirmed the boss spell-capture "skips power drops"
     * when P1 is full (only point items, no power, no cherry) — and the 0/2/7-only diag
     * stayed SILENT at capture, so the power items never reach this spawner (skipped
     * UPSTREAM) OR are a type the old filter missed (notably 4 = full-power "F"). Log
     * the first ~60 spawns of EVERY type with the spellcard flag so one capture run
     * shows exactly what the boss emits: a type-4 line at capture => the refill is
     * full-power items skipped when full; only type 1/6 lines => the reward is genuinely
     * point-only here; NO line at all during the capture burst => the boss uses a
     * different spawner / the power drop is decided and dropped entirely upstream. */
    if (s_b2DiagN < 60 && type != 6) {   /* skip the type-6 bullet-cancel star spam */
        uint32_t res0 = *ADDR_RES_PTR;
        float p1pw = res0 ? *(float *)(res0 + RES_POWER) : -1.f;
        Log("B2 diag spawn #%d: type=%d mode=%d sc=%d capwin=%d suppress=%d p1pow=%.0f p2pow=%.0f diff=%u",
            s_b2DiagN, type, mode, SpellcardActive(), s_captureWin, g_b2Suppress, p1pw, s_p2Power,
            *(volatile uint32_t *)0x00626280);
        s_b2DiagN++;
    }
    if (g_b2Suppress) {
        /* Post-capture refuel (confirmed in-game + cross-checked vs EoSD
         * ECL_OPCODE_DROPITEMS, EclManager.cpp:802-817): the boss spell reward loops
         * SpawnItem; when P1 power < 128 it drops BIG/SMALL POWER, but when P1 is FULL it
         * substitutes POINT (type 1) for every power item — the decision is UPSTREAM of
         * this spawner, so the popcorn power-zero below never sees it as 0/2. Inside the
         * post-spell window, turn that point reward back into a power item so a below-full
         * P2 can refuel (crucial on Extra/Phantasm). P1 (full) collecting a power item
         * still scores on pickup, so P1 loses nothing meaningful; the window keeps normal
         * in-stage point items untouched.
         *
         * CRUCIAL: after type=0 we must FALL THROUGH to the power-zero wrapper, not call
         * the original directly — otherwise SpawnItem's own full-power->cherry conversion
         * (FUN_004326f0: round(P1pow)>=0x80 && type in {0,2} => 7) re-converts our type-0
         * straight into a CHERRY (the regression seen 2026-06-18). */
        if (s_captureWin > 0 && type == 1) {
            if (!s_b2RefuelLogged) {
                Log("B2: post-capture point reward -> power (P2 below full, capwin=%d)", s_captureWin);
                s_b2RefuelLogged = 1;
            }
            type = 0;   /* now flows into the power-zero block below */
        }
        if (type == 0 || type == 2) {
            uint32_t res = *ADDR_RES_PTR;
            if (res) {
                volatile uint32_t *pw = (volatile uint32_t *)(res + RES_POWER);
                uint32_t saved = *pw;
                int rr;
                *pw = 0;            /* 0.0f ⇒ round(power)=0 ≤ 0x7f ⇒ engine keeps it power */
                rr = ((ItemSpawnFn_t)g_b2OrigItemSpawn)(self, edx, pos, type, mode);
                *pw = saved;        /* restore before any accessor checks the checksum     */
                g_b2Suppressed++;
                return rr;
            }
        }
    }
    return ((ItemSpawnFn_t)g_b2OrigItemSpawn)(self, edx, pos, type, mode);
}

/* Tier-1: scale enemy/boss HP cap by the active player count. On by default;
 * only takes effect while P2 is live (factor = 1 + (s_p2 != NULL)). F5 toggles. */
static int   s_bossHpScale = 1;
/* N2 — team DPS damper tuning + mode (coop.ini [coop] damper_boss_only, launcher box).
 * FLAT (0): every enemy *= 0.75. BOSS-ONLY (1): only lifebar enemies *= 0.60, popcorn
 * full damage. Constants are easy to retune from play feel. */
#define COOP_DAMPER_FLAT 0.75f
#define COOP_DAMPER_BOSS 0.60f
static int   s_damperBossOnly = 0;     /* 0 = flat 0.75 all; 1 = boss-only 0.60 */

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

/* ── crash breadcrumbs ──────────────────────────────────────────────────────────
 * A cryptic crash (the C1/C2 family) usually faults INSIDE a ZUN function we re-invoked
 * for P2, a few frames after the real cause, so the faulting EIP alone says little. We
 * keep a one-word breadcrumb of the last coop hook/section entered and which player it
 * was running for; the vectored exception handler (CoopCrashHandler) dumps it together
 * with the faulting context. Writing a pointer + int each hook entry is essentially free
 * and is the cheapest "what was coop doing when it died" signal. */
static volatile const char *s_crumb    = "init";   /* last hook/section reached      */
static volatile int         s_crumbWho = 0;         /* 0 = P1 / engine, 1 = P2 re-invoke */
static volatile uint32_t    s_crumbSeq = 0;         /* bumps every breadcrumb (liveness) */
#define CRUMB(name)       do { s_crumb = (name); s_crumbSeq++; } while (0)
#define CRUMB2(name, who) do { s_crumb = (name); s_crumbWho = (who); s_crumbSeq++; } while (0)

static int   s_readyFrames = 0;        /* consecutive P1-update frames seen        */
static int   s_autoSpawned = 0;        /* one-shot auto-spawn latch               */
static int   s_suppressP2  = 0;        /* DIAGNOSTIC: never spawn P2 (desync isolation) */
static int   s_b2CherryBothFull = 1;   /* B2: power->cherry only when BOTH full (FPU asm; default ON since x87 desync fix) */
static int   s_fpuGuard    = 0;        /* §8o: firewall the netcode's x87 FPU use (desync fix test) */
/* §8r: auto-recover a sustained in-stage desync via the th06 resync handshake + a
 * mid-stage RNG reseed. DEFAULT OFF — a 2-PC test (2026-06-18) PROVED it does NOT fix
 * PCB's desync: the handshake fires correctly on both sides but the divergence is an
 * ongoing GAME-STATE fork (mid-stage, cross-platform Wine↔Windows FP), which a reseed
 * can't undo, so it re-desyncs within ~200 frames and the friend found it WORSE than the
 * manual retry. Kept (inert) only because the protocol is a correct building block; the
 * recovery path that actually works is a full scene RESTART (Escape→Give up→Retry). */
static int   s_netAutoResync = 0;      /* §8r: OFF by default — proven not to fix PCB's mid-stage fork */
#define COOP_RESYNC_THRESHOLD 180      /* sustained desync frames (~3s) before auto-resync fires */
static int   s_diffForcedLogged = 0;   /* §8t: one-shot log when the guest first pins difficulty to host's */
#define AUTO_SPAWN_AFTER 2             /* frames of P1 in state 0 after the stage
                                          fly-in, then spawn P2 (item 2: spawn P2
                                          as good as immediately with P1 — was 30).
                                          Stays frame-counted, so it's deterministic
                                          on both machines under netplay. (True
                                          co-fly-in would need P2 its own fly-in;
                                          deferred — this just removes the ~0.5s gap.) */

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
#define OFF_HOMING_X   0x2428          /* float[3] homing target x,y,z           */
#define OFF_AIM_X      0x2434          /* float[3] SakuyaA aim target x,y,z      */
#define OFF_TGT_VALID  0x2440          /* int target-valid flag                  */
#define OFF_POS_Z      0x938           /* float player Z (OFF_POS_X + 8)         */

/* ---- PER-PLAYER aim/homing SOURCE (handoff §8c) ----
 * The block above is filled by the enemy update (FUN_00420620, PCBdecomp.c
 * 12904-12943) using the STATIC P1's position (DAT_004be408/40c/410) and written
 * only into P1's +0x2428 block, then mirrored to P2 each frame. So P2's homing
 * amulets / SakuyaA aimed knives chase whatever was nearest/in-cone for P1.
 * To make P2 choose targets relative to ITS OWN position we replicate that exact
 * acquisition for P2 (BuildP2TargetBlock) over a per-frame snapshot of the
 * targetable enemies. We get the snapshot for FREE from HookedDamage: the enemy
 * update calls the shot-damage sweep FUN_0043d9e0(enemy+0x2b0c, ...) once per
 * damageable enemy, so the `pos` arg points at enemy+0x2b0c -> enemy base =
 * pos - 0x2b0c. This avoids reverse-engineering the enemy-manager base out of the
 * task system (a wrong pointer walk would crash). It does miss enemies that are
 * on-screen but NOT currently damageable (e.g. a boss mid-invuln) -> for those
 * frames P2's acquisition finds nothing and we fall back to mirroring P1's block,
 * so behaviour is never worse than the old full mirror.
 *
 * Enemy struct fields (docs/th07_enemy_system.md): pos float[3] @+0x2b0c; state
 * byte @+0x2e29 with bit6 (0x40) = boss/has-lifebar (gates the nearest-x branch;
 * non-bit6 popcorn uses the lowest-enemy / in-cone fallback, same as ZUN). */
#define ENEMY_OFF_POS   0x2b0c
#define ENEMY_OFF_FLAGS 0x2e29
#define ENEMY_FLAG_BOSS 0x40
#define COOP_MAX_ENEMY_SNAP 256        /* damageable enemies per frame (bounded)  */
/* SakuyaA upward aim cone: ZUN tests atan2(dy,dx) in [-120deg,-60deg] (±30deg of
 * straight up). Expressed geometrically (no atan2 dep): enemy above (dy<0) and
 * |dx| within tan(30deg) of |dy|. */
#define COOP_AIM_CONE_TAN30 0.57735027f
static void *s_enemySnap[COOP_MAX_ENEMY_SNAP]; /* enemy base ptrs, this frame     */
static int   s_enemyCount = 0;
static int   s_perPlayerAim = 1;       /* per-player homing/aim source (default on)*/
/* P2 bomb-declaration portrait suppression (different-char P2); see the hooks
 * near HookedDamage. Declared here so DespawnP2 (earlier in the file) can reset. */
static int   s_hideP2Portrait   = 1;   /* hide P2's wrong bomb face (default on)   */
static int   s_declSuppressFace = 0;   /* live declaration is a different-char P2's*/
static int   s_declSuppressLogged = 0;
/* §8b diagnostic: instrumented P2 face-anm load (coop.ini [coop] p2_face_diag).
 * Default off; when on, probes the correct-face load once per P2 char and logs
 * who corrupts the 0x400 player window (see LoadP2FaceDiag). Pure read-only. */
static int   s_p2FaceDiag       = 0;

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
/* ⚠️ MISNOMER (kept for churn-safety): these four are NOT speed — they are the
 * player's HIT/GRAZE half-extents (the engine recomputes the AABB edges each
 * frame from player center ∓ these): +0x990/+0x994 = hit half-X/Y, +0x99c/+0x9a0 =
 * graze half-X/Y. ApplyP2Selection sets them from the .sht (sht[0xc]/2 hit,
 * sht[0x10]/2 graze) — semantically correct for P2's hitbox, so behaviour is fine;
 * only the names mislead. See docs/th07_player_struct.md "Open / unverified".
 * Do NOT reuse these as a movement-speed field — that field is unconfirmed. */
#define OFF_SPD_UNF_CUR   0x990       /* hit half-extent X (cur)   */
#define OFF_SPD_UNF       0x994       /* hit half-extent Y = sht[+0xc]/2   */
#define OFF_SPD_FOC_CUR   0x99c       /* graze half-extent X (cur) */
#define OFF_SPD_FOC       0x9a0       /* graze half-extent Y = sht[+0x10]/2 */
#define OFF_DEATH_TIMER   0x23f8      /* int death/deathbomb-window countdown (NOT
                                       * a hitbox): max while alive, decremented
                                       * while dying, finalizes death at 0. See
                                       * docs/th07_player_shot_bomb_system.md §5. */
static int   s_p2Sel = -1;             /* P2's sel 0-5; -1 = mirror P1. F3 toggles A/B */
/* Per-CHARACTER starting bomb stock (user/PCB spec): Reimu 3, Marisa 2, Sakuya 4.
 * P2 seeds its own bombs from this (by its character) on a fresh game instead of
 * copying P1's count; resources still carry unchanged across stage transitions. */
static const float kCharStartBombs[3] = { 3.0f, 2.0f, 4.0f };
static void *s_shtCache[6][2];         /* loaded .sht pairs (game heap, kept for life) */
static unsigned char s_selSaved[3];    /* global-swap save slots                  */
static int   s_selSwapped = 0;
static int   s_allowDiffChar = 0;      /* F2 enables cycling P2 to a DIFFERENT char */

/* ---- EoSD-style MENU character select (handoff §5g) ----
 * P1 picks char+type on the normal screen; instead of starting, we hold on
 * character-select and let P2 pick its own char+type, then start. We hook the
 * menu screen dispatcher FUN_004554d6 (ECX = menu object), drive a small FSM,
 * and route P2's input into the menu during its pass. On P2's commit we record
 * its selection into s_p2Sel/s_allowDiffChar (the in-stage auto-spawn machinery
 * then loads its char anm + bakes the loadout — a clean stage-start load, no
 * live entities, which is what the round-13 regression demanded) and restore
 * P1's selection globals so the engine inits the static player as P1.
 *
 * Menu state word (menu+0xd0f8): char-select active 5(normal)/9(extra)/0xd(prac),
 * shot-select 6/0xa/0xe (the 4/5/6 vs 8/9/0xa vs 0xc/0xd/0xe triples are
 * normal/extra/practice). We engage for normal (4/5/6) + practice (0xc/0xd/0xe)
 * only; extra is single-char, left vanilla (P2 just mirrors P1 there).
 * Menu input word DAT_004b9e4c (vs prev DAT_004b9e54, screens fire on rising
 * edge): up 0x10, down 0x20, left 0x40, right 0x80, confirm 0x1001, cancel 0xa. */
#define ADDR_MENU_DISPATCH  ((LPVOID)0x004554d6)            /* __fastcall(ecx=menu) */
#define ADDR_MENU_IN_CUR    ((volatile uint16_t *)0x004b9e4c)
#define ADDR_MENU_IN_PREV   ((volatile uint16_t *)0x004b9e54)
#define ADDR_MODE_FLAGS     ((volatile uint32_t *)0x0062f648) /* bit0 = practice    */
#define MENU_OFF_STATE      0xd0f8     /* screen-state word (param_1[0x343e])      */
#define MENU_OFF_SUBSTATE   0x8        /* param_1[2]: 0 anim-in, 1 active          */
#define MENU_OFF_CURSOR     0x0        /* param_1[0]: cursor index                 */
#define MENU_OFF_PREVSTATE  0x64       /* FUN_00455435 stashes the old state here  */
#define MENU_OFF_COUNTER3   0xc        /* param_1[3]                               */
#define MENU_OFF_FRAMECTR   0xd0fc     /* param_1[0x343f]: per-screen frame ctr    */
#define MENU_OFF_COUNTER2   0xb0c0     /* param_1[0x2c30]                          */
#define MENU_CONFIRM        0x1001
#define MENU_CANCEL         0xa
/* On-screen "P2 SELECT" prompt position during P2's char/shot pass (handoff §8d).
 * Drawn via the global ascii queue (FUN_00402060), which renders every scene —
 * including the front-end menu — so the cue is visible while P2 picks. The menu
 * uses the full 640px width; this sits in the upper-left, clear of the right-side
 * char portraits. Coords are a best guess (no live test this session) — tune to
 * taste after a visual check. */
#define MENU_PROMPT_X       64.f
#define MENU_PROMPT_Y       40.f
enum { CM_IDLE = 0, CM_P2_CHAR, CM_P2_SHOT, CM_COMMIT };
static int      s_coopMenu  = CM_IDLE; /* coop char-select FSM state               */
static int      s_menuSelect = 1;      /* feature enable (0 = bypass, vanilla menu)*/
static int      s_p1Char = 0, s_p1Type = 0; /* P1's pick, saved at its commit      */
static uint16_t s_menuPrev = 0;        /* prev combined menu-input for P2's pass   */
typedef int (__fastcall *MenuDispFn_t)(void *menu);
static MenuDispFn_t s_origMenuDispatch = NULL;

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
#define ANM_REV_TBL       0x2b6f0      /* mgr+: reverse base per id (bind reads it)*/
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
static uint32_t s_p2AnmFile = 0;       /* slot's file ptr at load; detect engine reuse */
static int      s_p2AnmActive = 0;     /* P2 currently uses a different-char anm       */
static int      s_p2IdCount = 0;       /* how many ids P2's char defines in the window */
static uint16_t s_p2Ids[ANM_MAX_IDS];                 /* the ids P2 defines            */
static uint32_t s_p2Script[ANM_MAX_IDS];              /* P2's script ptr per id        */
static uint32_t s_p2Rev[ANM_MAX_IDS];                 /* P2's reverse-base per id      */
static unsigned char s_p2Sprite[ANM_MAX_IDS][ANM_SPRITE_STRIDE]; /* P2's sprite def    */
static int      s_anmSwapped = 0;

/* Slots the engine itself loads into via FUN_0044df90 (the ONLY anm loader; the
 * load FREES the slot first, so reusing any of these would yank P2's anm out
 * from under our captured script/sprite pointers -> dangling deref crash). We
 * must load P2's char into a slot NOT in this set. 0x30 is the highest one the
 * game never touches; 0x31 is the staff-roll, 0x2e/0x2a/0x20 are stage assets. */
static const unsigned char kGameAnmSlots[] = {
    0x00,0x01,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,
    0x0f,0x10,0x11,0x12,0x13,0x15,0x17,0x18,0x19,0x1c,
    0x20,0x2a,0x2e,0x31
};
static int IsGameAnmSlot(int id)
{
    int i;
    for (i = 0; i < (int)(sizeof kGameAnmSlots); i++)
        if (kGameAnmSlots[i] == id) return 1;
    return 0;
}

static void Log(const char *fmt, ...)
{
    if (!s_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(s_log, fmt, ap);
    va_end(ap);
    fputc('\n', s_log);
    fflush(s_log);
}

/* Vectored exception handler: turn a silent crash into a log line. Fires for the whole
 * process, so we only report genuinely fatal codes (not the C++ EH / debug-print pseudo
 * exceptions the game/D3D raise normally), dump the faulting context + our breadcrumb,
 * then EXCEPTION_CONTINUE_SEARCH so the OS crash path proceeds unchanged (we never
 * swallow it). Re-entry-guarded in case the handler itself faults. The game has no ASLR
 * (fixed 0x400000 base), so a raw EIP/target like 0x0043d9e0 is directly greppable
 * against PCBdecomp / objdump; >= 0x10000000 is typically our DLL or a wild pointer. */
static LONG WINAPI CoopCrashHandler(EXCEPTION_POINTERS *ep)
{
    static volatile LONG s_inHandler = 0;
    DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    switch (code) {                       /* only the fatal ones — ignore EH/debug noise */
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (InterlockedExchange(&s_inHandler, 1)) return EXCEPTION_CONTINUE_SEARCH;

    if (s_log) {
        CONTEXT *c = ep->ContextRecord;
        EXCEPTION_RECORD *r = ep->ExceptionRecord;
        Log("====================  CRASH  ====================");
        Log("exception 0x%08lx at EIP=0x%08lx   crumb=\"%s\" who=%s seq=%u frame=%d",
            (unsigned long)code, (unsigned long)(c ? c->Eip : 0),
            s_crumb ? s_crumb : "?", s_crumbWho ? "P2" : "P1", s_crumbSeq, s_lastLogicFrame);
        if (code == EXCEPTION_ACCESS_VIOLATION && r && r->NumberParameters >= 2)
            Log("  access violation: %s 0x%08lx",
                r->ExceptionInformation[0] == 1 ? "WRITE to" :
                r->ExceptionInformation[0] == 8 ? "EXECUTE at" : "READ from",
                (unsigned long)r->ExceptionInformation[1]);
        if (c)
            Log("  EAX=%08lx EBX=%08lx ECX=%08lx EDX=%08lx\n"
                "  ESI=%08lx EDI=%08lx EBP=%08lx ESP=%08lx",
                (unsigned long)c->Eax, (unsigned long)c->Ebx, (unsigned long)c->Ecx,
                (unsigned long)c->Edx, (unsigned long)c->Esi, (unsigned long)c->Edi,
                (unsigned long)c->Ebp, (unsigned long)c->Esp);
        Log("  coop: p2=%p ghost=%d anmActive=%d inP2Update=%d  (P1 base=0x%08x)",
            (void *)s_p2, s_p2Ghost, s_p2AnmActive, s_inP2Update, ADDR_PLAYER_BASE);
        Log("=================================================");
    }
    s_inHandler = 0;
    return EXCEPTION_CONTINUE_SEARCH;     /* let the real crash path run */
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

/* P2 local-coop binds (item 4): WASD move (left hand), right hand on the actions.
 * WASD chosen over IJKL per player feedback ("WASD is free real estate"). P1 uses
 * arrows+Z/X/Shift, so W/A/D don't collide; S/D do alias menu keys (0x400/0x2000)
 * but gameplay reads only the low 9 bits, and the menu pass is overridden per-player.
 *
 * Each bind is a Win32 virtual-key code, overridable per-key from coop.ini [coop]
 * (p2_up/p2_down/p2_left/p2_right/p2_shoot/p2_focus/p2_bomb — see ParseVk). The
 * SHIPPED DEFAULTS move FOCUS+BOMB to the adjacent O/P pair (was U/O, split by I)
 * so a two-finger right hand reaches both, per the user's "OP" ask. */
enum { P2K_UP, P2K_DOWN, P2K_LEFT, P2K_RIGHT, P2K_SHOOT, P2K_FOCUS, P2K_BOMB, P2K_COUNT };
static int s_p2Keys[P2K_COUNT] = { 'W', 'S', 'A', 'D', VK_SPACE, 'O', 'P' };

/* Parse a coop.ini key token into a virtual-key code, falling back to `def` on an
 * empty/unknown token. Accepts: a single letter/digit (VK == uppercase ASCII for
 * A-Z/0-9), a friendly name (SPACE, COMMA, PERIOD, SEMICOLON, SLASH, LBRACKET,
 * RBRACKET, MINUS, PLUS, SHIFT, CTRL, ALT, ENTER, TAB, UP/DOWN/LEFT/RIGHT), or a
 * raw number ("0xBC" / "188") for any VK not otherwise named. */
static int ParseVk(const char *s, int def)
{
    static const struct { const char *n; int vk; } tbl[] = {
        {"SPACE",VK_SPACE},{"COMMA",VK_OEM_COMMA},{"PERIOD",VK_OEM_PERIOD},
        {"SEMICOLON",VK_OEM_1},{"SLASH",VK_OEM_2},{"LBRACKET",VK_OEM_4},
        {"RBRACKET",VK_OEM_6},{"MINUS",VK_OEM_MINUS},{"PLUS",VK_OEM_PLUS},
        {"SHIFT",VK_SHIFT},{"CTRL",VK_CONTROL},{"ALT",VK_MENU},{"ENTER",VK_RETURN},
        {"TAB",VK_TAB},{"UP",VK_UP},{"DOWN",VK_DOWN},{"LEFT",VK_LEFT},{"RIGHT",VK_RIGHT},
    };
    int i;
    if (!s || !s[0]) return def;
    if (!s[1]) {                                   /* single character */
        char c = s[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        return (unsigned char)c;                   /* A-Z / 0-9 / SPACE map VK==ASCII */
    }
    if (s[0] >= '0' && s[0] <= '9')                /* multi-char number -> raw VK */
        return (int)strtol(s, NULL, 0);
    for (i = 0; i < (int)(sizeof(tbl)/sizeof(tbl[0])); i++)
        if (_stricmp(s, tbl[i].n) == 0) return tbl[i].vk;
    return def;
}

static uint16_t ReadP2InputLocal(void)
{
    uint16_t w = 0;
    if (Down(s_p2Keys[P2K_UP]))    w |= IN_UP;
    if (Down(s_p2Keys[P2K_DOWN]))  w |= IN_DOWN;
    if (Down(s_p2Keys[P2K_LEFT]))  w |= IN_LEFT;
    if (Down(s_p2Keys[P2K_RIGHT])) w |= IN_RIGHT;
    if (Down(s_p2Keys[P2K_SHOOT])) w |= IN_SHOOT;
    if (Down(s_p2Keys[P2K_FOCUS])) w |= IN_FOCUS;
    if (Down(s_p2Keys[P2K_BOMB]))  w |= IN_BOMB;
    return w;
}
static int s_p2InputLogged = 0;

/* ======================================================================== *
 *  NETPLAY (fork A, single-DLL integration — docs/th07_fork_a_integration.md §8)
 *
 *  Wires the engine-agnostic netcode core (src/netplay/, exposed through the
 *  C-linkage shim netcode_c_api.h) straight into this DLL, so P2's input comes
 *  from the WIRE instead of the local keyboard. One UDP peer per process:
 *  host = P1's machine, guest = P2's machine. Delay-based lockstep, one 16-bit
 *  word carries both players (P1 = low 9 bits native, P2 = high 7 bits) kept in
 *  sync by an RNG-seed compare every frame (netcode.cpp / merge.cpp, already
 *  unit- + lockstep-tested by tests/).
 *
 *  DESIGN (the §8 "one seam owns the frame counter + both input globals"):
 *   - Detour FUN_00437c70 (HookedSceneTick) — the per-logic-frame scene-input
 *     task that runs in BOTH menus and gameplay (the th07 analog of th06's
 *     Supervisor::OnUpdate). It owns the single DLL frame counter. After ZUN
 *     polls (g_InputMenu = Input_Poll()), we replace g_InputMenu with the
 *     lockstep-MERGED word. In a stage the engine's own FUN_00442cd0 then copies
 *     g_InputMenu -> g_InputGameplay for free, so P1 (low bits) becomes the
 *     merged P1; P2's piggyback update reads the high bits (UnpackP2).
 *   - Detour FUN_00442c60 (HookedGameStart) — force the shared RNG seed before
 *     ZUN snapshots it (mirrors ZUN's own replay seed-restore; idempotent on the
 *     mid-stage re-fires noted in fork_a §3).
 *
 *  SAFETY: everything here is gated on s_netActive, which is ONLY set when
 *  coop.ini's [net] enabled=1 AND the transport came up. With netplay OFF
 *  (the default) NOTHING below runs — the confirmed-good local-keyboard co-op
 *  baseline is byte-for-byte unchanged.
 *
 *  PER-PLAYER SELECT (over the wire): the two-pass char-select FSM RUNS under
 *  netplay — P1's pass is driven by s_netP1Menu and P2's by s_netP2Menu (the
 *  de-merged per-player words from Nc_GetLastSplit), so each player picks its own
 *  character+shot deterministically on both machines. P1 leads the front-end until
 *  it locks its shot, then P2 picks. (Title/difficulty still navigate together via
 *  the UI-union word.) NOT YET NETWORK-TESTED — compile + netcode-unit verified
 *  only; see the handoff for the in-game test checklist.
 * ======================================================================== */
#include "netcode_c_api.h"

#define ADDR_SCENE_TICK ((LPVOID)0x00437c70)   /* per-frame scene input task (A1 seam) */
#define ADDR_GAME_START ((LPVOID)0x00442c60)   /* game/stage-start init (seed snapshot)*/
#define ADDR_INPUT_MENU ((volatile uint16_t *)0x004b9e4c) /* g_InputMenu (raw poll)    */
#define ADDR_RNG_SEED   ((volatile uint16_t *)0x0049fe20) /* g_RngState.seed           */
#define ADDR_RNG_CTR    ((volatile uint32_t *)0x0049fe24) /* g_RngState.call_counter   */
#define ADDR_RNG_FN     ((LPVOID)0x00431870)              /* FUN_00431870 — the LCG draw (§8al diag) */
#define ADDR_DIFFICULTY ((volatile uint32_t *)0x00626280) /* 0..3 main, 4 Extra, 5 Phantasm */
/* Saved config (th07.cfg) defaults — the difficulty-SELECT screen seeds its cursor from
 * the saved default difficulty (PCBdecomp:37150), and the live 0x626280 is written from
 * that cursor at confirm (:37675/:37795). These differ per install, so the desync is at
 * the SOURCE; §8ag resets the saved default to a common value at link-up. (Adjacent
 * config bytes: 0x575a84 = starting lives, 0x575a85 = bombs — same per-install class.) */
#define ADDR_CFG_DIFFICULTY ((volatile uint8_t *)0x00575a89) /* saved default difficulty 0..5 */
#define ADDR_CFG_LIVES      ((volatile uint8_t *)0x00575a84) /* saved "Initial Players" 0..4   */
#define ADDR_CFG_BOMBS      ((volatile uint8_t *)0x00575a85) /* saved starting bombs 0..3      */
/* Menu key-REPEAT (hold-to-scroll) state. ZUN's scene-tick (FUN_00437c70 @22803-22816)
 * derives these from the LOCAL keyboard poll, BEFORE our merged-word overwrite — so the
 * cursor's hold-scroll (gated on the repeat flag @PCBdecomp.c:29077) advances a machine-
 * dependent number of steps and the two peers land on DIFFERENT difficulty/cursor rows
 * (confirmed: one HOST on Hard, one on Phantasm). We recompute both from the MERGED stream
 * each menu frame so hold-scroll is deterministic. (Taps already sync: they edge-detect
 * cur/prev, both of which carry the merged word.) */
#define ADDR_MENU_REPEAT_FLAG ((volatile uint16_t *)0x004b9e5c) /* DAT_004b9e5c: repeat tick */
#define ADDR_MENU_REPEAT_CTR  ((volatile uint16_t *)0x004b9e60) /* DAT_004b9e60: hold counter */
static uint16_t s_menuRepeatCtr = 0;   /* our shadow of the repeat counter, merged-driven */

typedef int (__fastcall *SceneTickFn_t)(void *self);
typedef int (__fastcall *GameStartFn_t)(int self);
static SceneTickFn_t s_origSceneTick = NULL;
static GameStartFn_t s_origGameStart = NULL;

/* §8ag: Extra/Phantasm unlock predicates (the menu calls these to decide whether to OFFER
 * the option). Both take the score-data object in ecx. Under netplay we force them to
 * "unlocked" so the guest's menu offers the same options as the host's regardless of each
 * install's score.dat — required so the guest can follow the host into Extra/Phantasm.
 * FUN_0042f8de = Extra (any char cleared Easy..Lunatic; gates the Extra menu item,
 * PCBdecomp:36471/36557). FUN_0042f94c = Phantasm (PCBdecomp:18767; char-select/title). */
#define ADDR_EXTRA_UNLOCKED ((LPVOID)0x0042f8de)
#define ADDR_PHANT_UNLOCKED ((LPVOID)0x0042f94c)
typedef int (__fastcall *UnlockFn_t)(int self);
static UnlockFn_t s_origExtraUnlocked = NULL;
static UnlockFn_t s_origPhantUnlocked = NULL;

/* config (coop.ini [net], read once at attach) */
static int      s_netEnabled = 0;          /* feature flag (default OFF)              */
static int      s_netIsHost  = 1;
static char     s_netPeer[64]= "127.0.0.1";
static int      s_netPort    = 47000;
static int      s_netLocal   = 47001;
static int      s_netDelay   = 2;
static uint16_t s_netSeed    = 0x1234;

/* live state */
static int      s_netStarted = 0;          /* transport up; handshake in progress     */
static int      s_netVerWarned = 0;        /* logged a peer version mismatch once      */
static int      s_netActive  = 0;          /* transport up + connected               */
static int      s_netFrame   = 0;          /* DLL-owned logic-frame index             */
static uint16_t s_netMerged  = 0;          /* this frame's merged word (P2 reads high) */
static int      s_netSync    = 1;          /* last seen Nc_IsSync()                    */
static int      s_netDesyncLogged = 0;     /* nonzero = inside a desync episode        */
static int      s_netDesyncRun = 0;        /* consecutive in-stage sync=0 frames (fork)*/
static int      s_netHardDesyncLogged = 0; /* latch: sustained (real) desync reported  */
#define HARD_DESYNC_FRAMES 180             /* ~3s of unbroken in-stage sync=0 = a real
                                              fork, vs the ≤15-frame blips that self-heal
                                              at scene/seed-reforce boundaries (§8r) */
static int      s_netSyncRun  = 0;         /* consecutive in-sync gameplay frames      */
static int      s_netWaitMs   = 0;         /* last frame's lockstep wait (ms)          */
static uint16_t s_netSelfRng  = 0;         /* our RNG seed vs the peer's, last compared*/
static uint16_t s_netRcvRng   = 0;
static int      s_netStallLogged = 0;      /* throttle: frames since last STALL log    */
static int      s_netStatLogged  = 0;      /* throttle: frames since last status log   */
static int      s_netPeerLost = 0;         /* latched once the lockstep timed out      */
static FILE    *s_trace = NULL;            /* DIAGNOSTIC: per-frame determinism trace  */
static int      s_seedForced = 0;          /* seed forced once for the current scene   */
static int      s_netSceneId  = -1;        /* last top-level scene id (self+0x154); a
                                              change re-zeros the lockstep (start barrier)*/
static int      s_proxFade   = 1;          /* coop.ini [coop] proximity_fade (default ON) */
static int      s_disableDemo = 1;         /* coop.ini [coop] disable_demo (default ON)    */
static int      s_debugKeys  = 0;          /* coop.ini [coop] debug_keys (item 5): the F2-F12
                                              dev hotkeys (spawn/despawn/char-swap/killable…),
                                              which can desync a net game. Default OFF so an
                                              uninitiated player can't fork the sim by accident. */
static int      s_forceReplayP2 = -1;      /* coop.ini [coop] force_replay_p2: force co-op
                                              playback of a pre-tag-fix replay (-1 = off). */
static int      s_replayFpuPin = -1;       /* coop.ini [coop] replay_fpu_pin: pin x87 to 24-bit/
                                              round-nearest during co-op replay PLAYBACK so the sim
                                              matches the netplay recording's pinned FP environment
                                              (§8aa). -1 = auto (pin iff the replay was recorded under
                                              netplay, header 0x5c bit0); 0 = never; 1 = always (for
                                              old netplay replays that predate the header flag). */
/* Replay-playback flags read in HookedReplayLoad; declared here (before HookedSceneTick, which
 * reads them for the §8aa playback FPU pin). IsReplayPlayback() is forward-declared for the same. */
static int      s_replayIsCoop = 0;        /* the loaded replay carries our co-op tag (§8ah: stageblock+0x28) */
static int      s_replayNetRecorded = 0;   /* §8aa: recorded under netplay (stageblock 0x2a bit1) */
static unsigned short s_replayInitSeed = 0;/* §8ai: recorded netplay init seed (stageblock+0x20)  */
static int      s_replaySeedArm = 0;       /* §8ai: re-force the seed at the first in-stage frame  */
static int      s_replayPerFrameSeed = 0;  /* §8aj: replay carries a per-frame seed in each entry's free flags half */
static unsigned s_rpySeedStored = 0;       /* §8aj: count of per-frame seeds written (record) — logged once */
static unsigned s_rpySeedRestored = 0;     /* §8aj: count of per-frame seeds restored (playback) — logged once */
static int      s_replayTrace = 0;         /* §8ac coop.ini replay_trace: per-frame record/playback CSV */
static int      IsReplayPlayback(void);
/* Per-player RAW menu words this frame (de-merged P1=host / P2=guest), for the
 * per-player char-select FSM under netplay. Both machines compute the same pair. */
static uint16_t s_netP1Menu  = 0;
static uint16_t s_netP2Menu  = 0;
static uint16_t s_p1MenuPrev = 0;          /* P1's menu prev word (netplay edge detect)*/

/* netcode host-environment callbacks. readLocalInput is called INSIDE
 * Nc_GetInputNet, BEFORE the scene-tick hook overwrites g_InputMenu — so it
 * still sees this frame's raw local poll. readRngSeed = the desync oracle. */
static unsigned short NetReadLocalInput(void) { return *ADDR_INPUT_MENU; }
static unsigned short NetReadRngSeed(void)    { return *ADDR_RNG_SEED;   }

/* DIAGNOSTIC: sink for the netcode's WIRE SEND/RECV lines into coop_log.txt. */
static void NetLogSink(const char *msg) { Log("netplay: %s", msg); }

/* Unpack P2 (merged high bits, NB_*2 = bit<<9) into the low-bit gameplay layout
 * the player update reads (the inverse of merge.cpp's host/guest P2 mapping). */
static uint16_t UnpackP2(uint16_t m)
{
    uint16_t w = 0;
    if (m & (1u << 9))  w |= IN_SHOOT;   /* NB_SHOOT2 */
    if (m & (1u << 10)) w |= IN_BOMB;    /* NB_BOMB2  */
    if (m & (1u << 11)) w |= IN_FOCUS;   /* NB_FOCUS2 */
    if (m & (1u << 12)) w |= IN_UP;      /* NB_UP2    */
    if (m & (1u << 13)) w |= IN_DOWN;    /* NB_DOWN2  */
    if (m & (1u << 14)) w |= IN_LEFT;    /* NB_LEFT2  */
    if (m & (1u << 15)) w |= IN_RIGHT;   /* NB_RIGHT2 */
    return w;
}

/* Pack P2's gameplay word (IN_* low-bit layout) into the merged high bits — the exact
 * inverse of UnpackP2. Used by the LOCAL co-op replay-record seam (HookedFrameTask) to
 * lay P2 into g_InputMenu's high 7 bits before ZUN records it, so a local .rpy carries
 * P2's input the same way a netplay .rpy does (where merge.cpp/SceneTick already merge). */
static uint16_t PackP2(uint16_t w)
{
    uint16_t m = 0;
    if (w & IN_SHOOT) m |= (1u << 9);
    if (w & IN_BOMB)  m |= (1u << 10);
    if (w & IN_FOCUS) m |= (1u << 11);
    if (w & IN_UP)    m |= (1u << 12);
    if (w & IN_DOWN)  m |= (1u << 13);
    if (w & IN_LEFT)  m |= (1u << 14);
    if (w & IN_RIGHT) m |= (1u << 15);
    return m;
}
/* P2's local-keyboard word captured at the record seam (HookedFrameTask) each frame and
 * reused by the player-update hook, so the word saved to the replay is byte-identical to
 * the one P2 actually acts on this frame (record == live == playback). */
static uint16_t s_p2LocalIn = 0;

static void LoadNetConfig(void)
{
    char ini[MAX_PATH], buf[64];
    snprintf(ini, sizeof(ini), "%scoop.ini", s_dir);
    /* [coop] options apply with or without netplay (read before the [net] gate).
     * proximity_fade defaults ON now — fading the overlapping player is the
     * intended co-op look; set proximity_fade=0 in coop.ini to disable. */
    s_proxFade = (int)GetPrivateProfileIntA("coop", "proximity_fade", 1, ini);
    s_disableDemo = (int)GetPrivateProfileIntA("coop", "disable_demo", 1, ini);
    /* §8b: instrumented diagnostic for the open correct-P2-face goal. Default OFF —
     * when 1, a different-char P2's stage-start loads its face anm into a spare slot,
     * logs the 0x400-corruption fingerprint §8b needs, then fully restores + frees.
     * The shipped suppression baseline is untouched whether this is on or off. */
    s_p2FaceDiag = (int)GetPrivateProfileIntA("coop", "p2_face_diag", 0, ini);
    /* item 5: dev F-key hotkeys behind a debug toggle (default OFF). */
    s_debugKeys = (int)GetPrivateProfileIntA("coop", "debug_keys", 0, ini);
    /* play back a co-op replay recorded before the tag fix (-1 = off). */
    s_forceReplayP2 = (int)GetPrivateProfileIntA("coop", "force_replay_p2", -1, ini);
    /* §8aa: pin x87 during co-op replay playback to match a netplay recording's FP env. */
    s_replayFpuPin = (int)GetPrivateProfileIntA("coop", "replay_fpu_pin", -1, ini);
    /* §8ac: per-frame record/playback determinism CSV (coop_rdt_rec/rpy.csv). Default 0. */
    s_replayTrace = (int)GetPrivateProfileIntA("coop", "replay_trace", 0, ini);
    /* item 4: per-key overrides for the local-coop P2 binds (blank => keep default). */
    {
        static const char *p2KeyIni[P2K_COUNT] = {
            "p2_up", "p2_down", "p2_left", "p2_right", "p2_shoot", "p2_focus", "p2_bomb"
        };
        char kb[32];
        int i;
        for (i = 0; i < P2K_COUNT; i++) {
            GetPrivateProfileStringA("coop", p2KeyIni[i], "", kb, sizeof(kb), ini);
            if (kb[0]) s_p2Keys[i] = ParseVk(kb, s_p2Keys[i]);
        }
    }
    /* N2: damper mode. 0 = flat 0.75 on all enemies; 1 = boss-only 0.60 (stage full). */
    s_damperBossOnly = (int)GetPrivateProfileIntA("coop", "damper_boss_only", 0, ini);
    /* DIAGNOSTIC: suppress the P2 entity entirely (no auto-spawn, F9 no-ops). Lets a
     * netplay run test whether the residual sim-determinism desync (handoff §8m) comes
     * from the grafted P2 entity: set suppress_p2=1 on BOTH machines — if the RNG
     * counter then stays locked, P2's graft is the culprit; if it still desyncs, the
     * cause is engine-side / a hook. Default 0. */
    s_suppressP2 = (int)GetPrivateProfileIntA("coop", "suppress_p2", 0, ini);
    /* B2: gate the power->cherry conversion on BOTH players full (else P1 alone
     * full starves P2's power). Installs an FP-safe naked detour on the spawner.
     * Default ON since 2026-06-17: it reads s_p2Power, which is now in sync after
     * the x87 desync fix, so the prior reason to keep it off is gone. Set
     * cherry_both_full=0 to restore vanilla (P1-full converts). */
    s_b2CherryBothFull = (int)GetPrivateProfileIntA("coop", "cherry_both_full", 1, ini);
    /* §8o follow-up: firewall the per-frame netcode's x87 FPU use (save/restore the
     * game's full x87 state around Nc_GetInputNet) so the lockstep wait's timing math
     * can't perturb ZUN's FP differently on the two machines. Candidate desync fix. */
    s_fpuGuard = (int)GetPrivateProfileIntA("coop", "fpu_guard", 1, ini);  /* CONFIRMED FIX — default ON */
    /* §8r: auto-resync — DEFAULT OFF. The 2-PC test proved a mid-stage reseed does not fix
     * PCB's desync (an ongoing cross-platform game-state fork, not a recoverable frame
     * offset); the manual scene RESTART is the working recovery. Left as an opt-in flag. */
    s_netAutoResync = (int)GetPrivateProfileIntA("coop", "auto_resync", 0, ini);
    s_netEnabled = (int)GetPrivateProfileIntA("net", "enabled", 0, ini);
    if (!s_netEnabled) return;
    GetPrivateProfileStringA("net", "role", "host", buf, sizeof(buf), ini);
    s_netIsHost = (_stricmp(buf, "guest") != 0);
    GetPrivateProfileStringA("net", "peer", "127.0.0.1", s_netPeer, sizeof(s_netPeer), ini);
    s_netPort  = (int)GetPrivateProfileIntA("net", "port",  47000, ini);
    s_netLocal = (int)GetPrivateProfileIntA("net", "local", 47001, ini);
    s_netDelay = (int)GetPrivateProfileIntA("net", "delay", 2, ini);
    GetPrivateProfileStringA("net", "seed", "0x1234", buf, sizeof(buf), ini);
    s_netSeed = (uint16_t)strtoul(buf, NULL, 0);
}

static void StartNet(void)
{
    if (!s_netEnabled) { Log("netplay: disabled (coop.ini [net] enabled=0) — local P2"); return; }
    Nc_SetCallbacks(NetReadLocalInput, NetReadRngSeed);
    Nc_SetLog(NetLogSink);
    /* §8r: opt the netcode into auto-resync (off by default in the core so the native
     * tests stay deterministic). Reseed-on-fire is handled in HookedSceneTick. */
    Nc_SetAutoResync(s_netAutoResync, COOP_RESYNC_THRESHOLD);
    int ok = s_netIsHost
        ? Nc_StartHost("", s_netPort, 2 /*AF_INET*/)
        : Nc_StartGuest(s_netPeer, s_netPort, s_netLocal, 2 /*AF_INET*/);
    if (!ok) { Log("netplay: transport start FAILED (role=%s) — falling back to local P2",
                   s_netIsHost ? "host" : "guest"); return; }
    /* Arm the handshake; the link goes live only once the peer answers (pumped in
     * HookedSceneTick). The HOST's delay+seed are pushed to the guest over the wire,
     * so the guest's ini seed= is ignored — no more hand-matching. */
    Nc_BeginHandshake(s_netDelay, s_netSeed);
    s_netStarted = 1;          /* pump the handshake each front-end frame             */
    Log("netplay: transport up role=%s peer=%s port=%d local=%d. Handshaking — "
        "waiting for the peer to answer (start the other machine)...",
        s_netIsHost ? "host" : "guest", s_netPeer, s_netPort, s_netLocal);
}

/* ── x87 FPU state probe + firewall (§8o follow-up) ────────────────────────────
 * Vanilla PCB replays sync across the host (Wine) and guest (Windows), so ZUN's
 * own FP is deterministic between these machines. Yet our netplay desyncs even
 * with P2 suppressed — pointing at something OUR per-frame netcode does to the
 * shared x87 state. The lockstep wait in Nc_GetInputNet busy-spins a
 * MACHINE-DEPENDENT number of times doing double timing math; if that leaves the
 * x87 control word / stack different on the two machines, ZUN's subsequent FP
 * (positions, aim) rounds differently and the sim forks. FpuCw/FpuSw read the
 * control + status words (status TOP field = stack depth) so we can diff them
 * host-vs-guest in the trace. FPU_SAVE/RESTORE wrap the netcode call so it can't
 * touch the game's FPU at all (gated by s_fpuGuard). */
static inline unsigned short FpuCw(void)
{ unsigned short w; __asm__ __volatile__("fnstcw %0" : "=m"(w)); return w; }
static inline unsigned short FpuSw(void)
{ unsigned short w; __asm__ __volatile__("fnstsw %0" : "=m"(w)); return w; }
/* Set the x87 control word for real (fldcw). _controlfp on this llvm-mingw CRT does NOT change
 * the x87 PRECISION field (confirmed §8ad: the det-trace stayed at 0x037f under the "24-bit pin"),
 * so any precision change must use fldcw directly. 0x007f = 24-bit/round-nearest (D3D/vanilla),
 * 0x037f = 64-bit extended (what the netcode leaves the x87 in during live netplay). */
static inline void FpuSetCw(unsigned short cw)
{ __asm__ __volatile__("fldcw %0" : : "m"(cw)); }
static unsigned char s_fpuState[108] __attribute__((aligned(16)));
#define FPU_SAVE()    __asm__ __volatile__("fnsave  %0" : "=m"(s_fpuState) : : "memory")
#define FPU_RESTORE() __asm__ __volatile__("frstor  %0" : : "m"(s_fpuState) : "memory")
static unsigned short s_fpuCw = 0, s_fpuSw = 0;   /* last-frame x87 words for the trace (s_fpuGuard declared above) */

/* ── x87 FPU control-word PIN (the likely real desync fix — from nightshift's
 *    keen-ramanujan branch, §8q) ────────────────────────────────────────────────
 * Direct3D8 sets the x87 to 24-bit single precision at device-create (ZUN omits
 * D3DCREATE_FPU_PRESERVE), so vanilla PCB — and its replays — run at 24-bit. But the
 * two players' D3D drivers / wrappers (Wine vs Windows, different GPUs) can leave the
 * control word in DIFFERENT states, and D3D can RESET it on Present(); so whether the
 * two machines' precision matches varies run-to-run — exactly the INTERMITTENT desync
 * (a clean run when they happen to match, "Desync City" when they don't). fnsave/
 * frstor (s_fpuGuard) can't fix this: it faithfully preserves whatever (wrong) word
 * the game already had. The pin FORCES precision=24-bit, rounding=nearest each logic
 * frame so both machines agree regardless of what D3D did. Gated on s_netActive — the
 * confirmed-good local/replay build never touches the control word. The first pin logs
 * the PREVIOUS word so a 2-PC test reveals whether host and guest actually differed. */
static int s_fpuPinned = 0;
static void PinFpuForNetplay(void)
{
    unsigned prev = _controlfp(0, 0);                   /* read current */
    _controlfp(_PC_24 | _RC_NEAR, _MCW_PC | _MCW_RC);   /* pin 24-bit single, round-nearest */
    if (!s_fpuPinned) {
        s_fpuPinned = 1;
        Log("netplay: x87 control word pinned to 24-bit/round-nearest (was 0x%04x). "
            "role=%s — if host and guest 'was' values differ, that was the desync.",
            prev & 0xffff, Nc_IsHost() ? "host" : "guest");
    }
}

/* §8aa: same pin, applied during co-op replay PLAYBACK. A netplay .rpy was recorded with
 * the FPU pinned (above), so its input stream only re-simulates faithfully if playback runs
 * in the same 24-bit/round-nearest environment. Without it, D3D can leave the control word in
 * a different precision/rounding than the recording used, so trig-driven aimed enemy shots
 * drift and the run desyncs after a while ("resembles the moves but not accurately"). Gated by
 * the header net-recorded flag (or replay_fpu_pin override) so solo/vanilla replays — recorded
 * WITHOUT a pin — keep their native FP and are never perturbed. */
static int s_fpuPinnedReplay = 0;
static int s_replayPinDecisionLogged = 0;   /* §8aa: one-shot per replay, logs pin yes/no + cw */
static unsigned short s_replayTargetCw = 0x007f;  /* §8ad: the x87 cw the recording's SIM ran at */
/* §8ad: reproduce the recording's x87 PRECISION on playback. The real bug: a NETPLAY recording's
 * sim runs at 64-bit (0x037f — the netcode leaves the x87 in extended precision), but replay
 * playback runs at D3D's 24-bit (0x007f, confirmed: coop_rdt_rpy.csv cw=007f vs the live
 * det-trace 037f). Same inputs + same seed at a DIFFERENT precision => the sim forks. Local
 * recordings have no netcode so they run at 24-bit and replay fine — which is exactly why local
 * replays were perfect and netplay replays desynced. Force the recorded precision with a REAL
 * fldcw (PinFpuForNetplay's _controlfp was a no-op). */
static unsigned short s_replayPinCw = 0;   /* §8ad: cw to pin playback to (0 = no pin this run) */
static void PinFpuForReplay(unsigned short cw)
{
    unsigned short prev = FpuCw();
    FpuSetCw(cw);
    if (!s_fpuPinnedReplay) {
        s_fpuPinnedReplay = 1;
        Log("replay PLAYBACK: x87 control word set to 0x%04x (was 0x%04x) — reproducing the "
            "recording's precision (§8ad).", cw, prev);
    }
}

/* DIAGNOSTIC (det-trace) — one CSV row per logic frame while netplay is active.
 * Both machines run the SAME frame indices under lockstep, so diffing the host
 * and guest CSVs by the `frame` column gives the exact frame the sims part ways.
 * The live RNG counter (0x0049fe24) is monotonic within a stage and is NOT reset
 * by our seed-force, so it catches divergence the per-frame seed (which the
 * seed-force pins back to initSeed) would otherwise mask. seed/counter are read
 * live here at the top of the logic frame (== end of the previous frame's sim),
 * which is the same relative sample point on both machines. */
static void NetTrace(int frame, int inStage, uint16_t merged, int waitMs, int sync)
{
    if (!s_trace) {
        char p[MAX_PATH];
        snprintf(p, sizeof(p), "%scoop_trace_%s.csv", s_dir,
                 Nc_IsHost() ? "host" : "guest");
        s_trace = fopen(p, "w");
        if (!s_trace) return;
        fputs("frame,inStage,merged,p1,p2,seed,counter,waitMs,sync,"
              "readFrame,selfKey,rcvKey,rcvStatus,rcvSrc,rcvWrites,fpucw,fpusw\n", s_trace);
    }
    /* netcode internals for this frame: the index GetKeys read (readFrame =
     * netFrame-delay) and the raw self/rcv words it merged. Diffing host vs guest
     * by readFrame separates a frame-index misalignment (readFrame differs at the
     * same row) from a stale/mis-delivered peer input (selfKey/rcvKey disagree at
     * the same readFrame). rcvStatus: 0=immediate 1=after-wait 2=timeout. */
    int rf = -1, rs = 0; unsigned short sk = 0, rk = 0;
    int rsrc = -1, rwr = 0;
    Nc_GetReadStats(&rf, &sk, &rk, &rs);
    Nc_GetRcvSrc(&rsrc, &rwr);
    fprintf(s_trace, "%d,%d,%04x,%04x,%04x,%04x,%u,%d,%d,%d,%04x,%04x,%d,%d,%d,%04x,%04x\n",
            frame, inStage, merged, s_netP1Menu, s_netP2Menu,
            (unsigned)*ADDR_RNG_SEED, (unsigned)*ADDR_RNG_CTR, waitMs, sync,
            rf, sk, rk, rs, rsrc, rwr, s_fpuCw, s_fpuSw);
    fflush(s_trace);                        /* survive the freeze tail */
}

/* §8ac REPLAY-DETERMINISM TRACE — one CSV row per logic frame, keyed by the REPLAY FRAME
 * INDEX (the record/playback task's own counter at *(*ADDR_REPLAY_MGR)), which is identical
 * during recording and playback. So a netplay (or local) record run and a playback of its .rpy
 * produce two files that diff line-for-line: the first rf where seed/counter/pos differ is the
 * exact divergence. input = g_InputGameplay (the merged word actually applied this frame), so
 * the streams can also be aligned by input if the rf origins differ. Gated by [coop] replay_trace
 * (default 0) so normal play writes nothing. Separate files for record vs playback. */
static FILE *s_rdtRec = NULL, *s_rdtRpy = NULL;
static void ReplayDetTrace(int playback)
{
    if (!s_replayTrace) return;
    void *rm = *ADDR_REPLAY_MGR;
    int rf = rm ? *(int *)rm : -1;
    FILE **fp = playback ? &s_rdtRpy : &s_rdtRec;
    if (!*fp) {
        char p[MAX_PATH];
        snprintf(p, sizeof(p), "%scoop_rdt_%s.csv", s_dir, playback ? "rpy" : "rec");
        *fp = fopen(p, "w");
        if (!*fp) return;
        /* §8ak diag: inMenu/netMerged/headW/headNext expose the P2 input-timing slip. P2 lags P1 by
         * one frame on playback — record sources P2 from the FRESH s_netMerged (merged[N]) while
         * playback sources it from g_InputGameplay; this records every candidate source at the SAME
         * sample point (pre-update) so a rec-vs-rpy diff shows which word P2 must use to match. */
        fputs("rf,input,seed,counter,p1x,p1y,p2x,p2y,cw,inMenu,netMerged,headW,headNext\n", *fp);
    }
    void *p2 = (void *)s_p2;
    /* replay buffer current head + next entry words (the recorded merged words FUN_00442ee0 applies) */
    unsigned hW = 0xFFFF, hN = 0xFFFF;
    if (rm) {
        unsigned char *head = *(unsigned char **)((char *)rm + 0x84);
        if (head) { hW = *(unsigned short *)head; hN = *(unsigned short *)(head + 4); }
    }
    fprintf(*fp, "%d,%04x,%04x,%u,%.3f,%.3f,%.3f,%.3f,%04x,%04x,%04x,%04x,%04x\n",
            rf, (unsigned)*ADDR_INPUT_GAMEPLAY,
            (unsigned)*ADDR_RNG_SEED, (unsigned)*ADDR_RNG_CTR,
            *(float *)((char *)ADDR_PLAYER_BASE + OFF_POS_X),
            *(float *)((char *)ADDR_PLAYER_BASE + OFF_POS_Y),
            p2 ? *(float *)((char *)p2 + OFF_POS_X) : -1.0f,
            p2 ? *(float *)((char *)p2 + OFF_POS_Y) : -1.0f,
            (unsigned)FpuCw(),
            (unsigned)*ADDR_INPUT_MENU, (unsigned)s_netMerged, hW, hN);
    fflush(*fp);
}

/* §8al RNG-CALLER TRACE — the within-frame divergence (the seed force-pins fine, but the rec sim drew
 * 24 more rands than the rpy sim on the same frame: an off-screen event — "item RNG" — fires on record
 * but not playback) needs to be attributed to a CODE SITE. Hook the LCG (FUN_00431870) and tally the
 * caller's return address into a PER-FRAME HISTOGRAM, flushed once per replay frame (separately for
 * record vs playback). A diff of coop_rngcall_rec.csv vs coop_rngcall_rpy.csv at the diverging rf shows
 * which function draws the extra rands. Gated by [coop] replay_trace, in-stage during a co-op run only.
 *
 * §8am: this was per-DRAW fprintf+fflush, which storms disk I/O whenever the draw rate spikes (P2 focus
 * → focus-fire/combat) → SEVERE FPS lag that even bakes into the netplay replay. The histogram keeps the
 * hot path to an in-memory tally (≤RNG_HIST_MAX linear scan) and writes ONE line per frame — no per-draw
 * I/O — so the diagnostic no longer perturbs the run, and a focus-triggered draw STORM (if that is what's
 * happening) shows up as one caller with a huge per-frame count, which is itself the lead. */
typedef unsigned short (__fastcall *RngFn_t)(void *seed);
static RngFn_t s_origRng = NULL;
static FILE *s_rngRec = NULL, *s_rngRpy = NULL;
#define RNG_HIST_MAX 96
static struct { unsigned caller; unsigned count; } s_rngHist[RNG_HIST_MAX];
static int s_rngHistN   = 0;
static int s_rngHistRf  = -1;
static int s_rngHistPlay = -1;
static void RngHistFlush(void)
{
    if (s_rngHistN <= 0) { s_rngHistN = 0; return; }
    FILE **fp = s_rngHistPlay ? &s_rngRpy : &s_rngRec;
    if (!*fp) {
        char p[MAX_PATH];
        snprintf(p, sizeof(p), "%scoop_rngcall_%s.csv", s_dir, s_rngHistPlay ? "rpy" : "rec");
        *fp = fopen(p, "w");
        if (*fp) fputs("rf,total,callers(addr:count;...)\n", *fp);
    }
    if (*fp) {
        unsigned total = 0; int i;
        for (i = 0; i < s_rngHistN; i++) total += s_rngHist[i].count;
        fprintf(*fp, "%d,%u,", s_rngHistRf, total);
        for (i = 0; i < s_rngHistN; i++)
            fprintf(*fp, "%08x:%u;", s_rngHist[i].caller, s_rngHist[i].count);
        fputc('\n', *fp);
        fflush(*fp);                       /* once per frame — cheap */
    }
    s_rngHistN = 0;
}
static unsigned short __fastcall HookedRng(void *seed)
{
    void *caller = __builtin_return_address(0);
    if (s_replayTrace) {
        int inStage = (int)(((*(volatile uint32_t *)0x0062f648) >> 2) & 1);
        if (inStage && (s_replayIsCoop || s_netActive || s_p2)) {
            int play = IsReplayPlayback();
            void *rm = *ADDR_REPLAY_MGR;
            int rf = rm ? *(int *)rm : -1;
            if (rf != s_rngHistRf || play != s_rngHistPlay) {  /* new frame -> flush the last one */
                RngHistFlush();
                s_rngHistRf = rf; s_rngHistPlay = play;
            }
            /* cap to early frames (the divergence shows up by rf~600) so files stay small */
            if (rf >= 0 && rf <= 4000) {
                unsigned c = (unsigned)(uintptr_t)caller;
                int i, found = 0;
                for (i = 0; i < s_rngHistN; i++)
                    if (s_rngHist[i].caller == c) { s_rngHist[i].count++; found = 1; break; }
                if (!found && s_rngHistN < RNG_HIST_MAX) {
                    s_rngHist[s_rngHistN].caller = c;
                    s_rngHist[s_rngHistN].count  = 1;
                    s_rngHistN++;
                }
            }
        }
    }
    return s_origRng(seed);
}

/* Re-derive ZUN's menu key-repeat (hold-to-scroll) from the MERGED word so it is
 * identical on both peers. Mirrors FUN_00437c70's own logic (PCBdecomp.c:22806-22816)
 * verbatim, but with prev = last frame's merged (ZUN already copied it into
 * ADDR_MENU_IN_PREV) and cur = this frame's merged — instead of the local poll ZUN used.
 * We keep our own shadow counter (ZUN's was already bumped from the local compare this
 * frame; we discard that by overwriting both globals). Menu-only — the flag is consumed
 * solely by the menu cursor; in-stage the merged word is gameplay input. */
static void SyncMenuRepeat(uint16_t merged)
{
    uint16_t prevMerged = *ADDR_MENU_IN_PREV;     /* = last frame's merged word */
    *ADDR_MENU_REPEAT_FLAG = 0;
    if (prevMerged == merged) {
        if (s_menuRepeatCtr > 0x1d) {
            *ADDR_MENU_REPEAT_FLAG = (uint16_t)(s_menuRepeatCtr % 8 == 0);
            if (s_menuRepeatCtr > 0x25) s_menuRepeatCtr = 0x1e;
        }
        s_menuRepeatCtr++;
    } else {
        s_menuRepeatCtr = 0;
    }
    *ADDR_MENU_REPEAT_CTR = s_menuRepeatCtr;
}

/* FUN_00437c70 — per-logic-frame scene input task. Owns the netcode frame
 * counter and overwrites g_InputMenu with the merged word. */
static int __fastcall HookedSceneTick(void *self)
{
    int r = s_origSceneTick(self);          /* ZUN: g_InputMenu = Input_Poll() */
    /* §8ad: co-op replay PLAYBACK runs at D3D's 24-bit x87, but a NETPLAY run RECORDED at 64-bit
     * (the netcode leaves the x87 in extended precision). Same inputs+seed at a different precision
     * forks the sim — THE netplay-replay desync. Reproduce the recorded precision: pin to
     * s_replayTargetCw (from the header) whenever it differs from the default 24-bit. Local replays
     * (target 0x007f) are left untouched (they already replay perfectly). Auto (replay_fpu_pin=-1)
     * decides by the target; =1 forces a pin, =0 disables. Real fldcw, before this frame's sim. */
    /* §8ad: apply the playback FPU pin if this scene-tick runs (menus). The DECISION is made in
     * HookedReplayLoad and the per-frame IN-STAGE pin is HookedPlayTask (FUN_00442ee0) — this
     * scene-tick does NOT run during in-stage replay playback (confirmed: no decision line fired). */
    if (IsReplayPlayback() && s_replayPinCw) FpuSetCw(s_replayPinCw);
    if (s_netStarted && !s_netActive) {
        /* connection handshake at the front-end: keep pinging until the peer answers,
         * then the link is live (guest has adopted the host's delay+seed). */
        if (Nc_PumpHandshake()) {
            s_netActive  = 1;
            s_netStarted = 0;               /* don't re-handshake on a later drop      */
            s_netFrame   = 0;
            /* anchor the scene-boundary detector to wherever we linked up (usually the
             * title menu) so the first active frame doesn't fire a spurious re-zero. */
            s_netSceneId = *(int *)((char *)self + 0x154);
            s_netDesyncLogged = 0; s_netSyncRun = 0; s_netPeerLost = 0;
            s_netDesyncRun = 0;    s_netHardDesyncLogged = 0;
            s_netStallLogged = 0;  s_netStatLogged = 0;
            s_seedForced = 0;               /* force the shared seed at the first stage */
            s_menuRepeatCtr = 0;            /* fresh hold-scroll shadow for lockstep menu */
            Nc_Reset();                     /* fresh lockstep maps from frame 0         */
            /* §8ag: HOST-AUTHORITATIVE START CONFIG, fixed at the SOURCE. Each install's
             * difficulty-SELECT cursor seeds from its SAVED default difficulty
             * (config 0x575a89; PCBdecomp:37150), which then writes the live 0x626280 at
             * confirm. Saved defaults differ per install, so lockstepped navigation
             * preserves the offset and the two CONFIRM different difficulties — and the
             * difficulty-derived run state latched at confirm then diverges, which the old
             * per-frame pin of the LIVE 0x626280 never fixed (it only corrected the value
             * the pause menu reads). Reset the saved default to Normal on BOTH machines —
             * the same realignment ZUN's Extra-Start path performs — so both cursors seed
             * identically; from here lockstep keeps 0x575a89 (and the confirmed difficulty)
             * in step. Link-up always precedes difficulty selection (g_InputMenu is pinned
             * to the startup menu below until now), so the reset can't clobber a live pick. */
            *ADDR_CFG_DIFFICULTY = 1;        /* Normal — common cursor seed on both peers */
            /* §8ag: starting lives + bombs are the same per-install config-desync class
             * (each menu seeds from these saved bytes). Reset both peers to PCB's defaults
             * so the two runs start with identical resources; players can change them
             * together in-menu afterwards (lockstepped, so they stay in step). */
            *ADDR_CFG_LIVES = 2;             /* default Initial Players */
            *ADDR_CFG_BOMBS = 3;             /* default starting bombs   */
            Log("netplay: LINK UP (handshake done). role=%s delay=%d seed=0x%04x. "
                "Both inputs from the WIRE; each player picks its own character. "
                "Start config reset on both: difficulty=Normal lives=2 bombs=3 (§8ag).",
                Nc_IsHost() ? "host" : "guest", Nc_GetDelay(), Nc_GetInitSeed());
        } else if (Nc_HandshakeVersionBad() && !s_netVerWarned) {
            Log("netplay: peer VERSION MISMATCH — both machines must run the same "
                "build. Staying on local P2.");
            s_netVerWarned = 1;
        }
        /* MENU FREEZE during the handshake: until the link is live, neither machine's
         * menu may move. ZUN's input task (s_origSceneTick above) already polled the
         * LOCAL keyboard into g_InputMenu; left alone, each machine navigates on its own
         * while it waits — and on a real connection that wait is SECONDS (the host sits in
         * the menu until the guest starts). Whatever each player does in that window moves
         * only the local cursor and is never reconciled, so when lockstep engages the two
         * are already on different items/sections (host Normal / guest Lunatic; P1 on the
         * difficulty list / P2 in Extra). Zeroing g_InputMenu pins BOTH at the identical
         * startup menu; once LINK UP fires, the merged word (below) drives them together
         * from a common state. (Loopback links up instantly, so this never bit there.) */
        *ADDR_INPUT_MENU = 0;
        return r;                           /* no lockstep until connected             */
    }
    if (s_netActive) {
        PinFpuForNetplay();   /* §8q: force matching x87 precision/rounding before the sim runs */
        /* DIFFICULTY (§8ag): synced at the SOURCE now — the link-up cursor reset above makes
         * both peers confirm the same difficulty, so the live 0x626280 is naturally identical
         * and we no longer FORCE it (the old per-frame pin only ever fixed the pause-menu value,
         * not the difficulty-derived run state, which is why gameplay still diverged). We still
         * report ours on the wire for a cross-check: if the two live difficulties ever disagree
         * once IN-STAGE, the cursor reset didn't hold — surface it ONCE rather than mask it.
         * (The per-scene SEEDFORCE log also prints diff= on both peers for the same audit.) */
        Nc_SetLocalDifficulty((int)*ADDR_DIFFICULTY);
        if (!s_netIsHost && ((*ADDR_MODE_FLAGS >> 2) & 1) && !s_diffForcedLogged) {
            int hd = Nc_GetPeerDifficulty();
            if (hd >= 0 && (uint32_t)hd != *ADDR_DIFFICULTY) {
                Log("netplay: WARNING difficulty MISMATCH in stage (self=%u host=%d) — §8ag "
                    "cursor reset did not hold; the run will desync, investigate",
                    (unsigned)*ADDR_DIFFICULTY, hd);
                s_diffForcedLogged = 1;
            }
        }
        /* START BARRIER / scene-reset — the th06 Supervisor::OnUpdate "last_frame_a >
         * frame_a" realign (Supervisor.cpp), driven here by PCB's top-level scene id
         * at self+0x154. th06's lockstep clock is the game's per-scene calcCount, which
         * the engine RESETS at every scene boundary; our port used a free-running
         * counter with no such reset, so the menu->stage load — where each machine
         * burns a DIFFERENT number of front-end ticks loading — drifted the two clocks
         * apart for good (the host-330 / guest-468 gap in the desync logs, then the 5s
         * stall + peer-loss). Detect the scene change and re-zero: clear the peer's
         * stale per-frame inputs and restart the new scene at logic-frame 0 on BOTH
         * machines, so every scene (and the stage) begins lockstep from the same point.
         * Both commit the menu->stage transition on the same input-driven logic-frame,
         * so the re-zero lands together; the seed is forced by HookedGameStart. */
        int sceneId = *(int *)((char *)self + 0x154);
        if (sceneId != s_netSceneId) {
            Log("netplay: scene %d -> %d — lockstep re-aligned to frame 0 (was %d)",
                s_netSceneId, sceneId, s_netFrame);
            s_netSceneId = sceneId;
            s_netFrame   = 0;
            s_menuRepeatCtr = 0;        /* fresh hold-scroll state for the new scene  */
            s_seedForced = 0;           /* re-force the shared seed at the new scene  */
            Nc_Reset();                 /* clear peer buffer + frame index (th06 reset) */
            s_netDesyncLogged = 0; s_netSyncRun = 0;
            s_netDesyncRun = 0;    s_netHardDesyncLogged = 0;
            s_netStallLogged  = 0; s_netStatLogged = 0;
        }
        int inStage = (int)((*ADDR_MODE_FLAGS >> 2) & 1);  /* recording active = in a stage */
        int ctrl = 0;
        /* FPU firewall: save ZUN's full x87 state, run the netcode (its lockstep wait
         * does double timing math + a machine-dependent spin count), then capture what
         * it left (for the trace) and restore the game's state so its FP is untouched. */
        if (s_fpuGuard) FPU_SAVE();
        unsigned short merged = Nc_GetInputNet(s_netFrame++, inStage ? 0 : 1, &ctrl);
        s_fpuCw = FpuCw(); s_fpuSw = FpuSw();   /* x87 words AFTER the netcode ran this frame */
        if (s_fpuGuard) FPU_RESTORE();
        s_netMerged = merged;

        /* §8r AUTO-RESYNC reseed: the netcode just realigned the lockstep at an agreed
         * frame (cleared the misaligned peer buffer on BOTH machines). Snap the game RNG
         * to the shared init seed on THIS frame so the two sims re-converge from here —
         * the automatic equivalent of the scene-reset SEEDFORCE a manual retry performs.
         * Both machines fire on the same lockstep frame, so they reseed together. */
        if (Nc_PollResyncFired()) {
            uint16_t before = *ADDR_RNG_SEED;
            *ADDR_RNG_SEED = Nc_GetInitSeed();
            s_seedForced = 1;     /* HookedGameStart already aligned; don't double-force */
            Log("netplay: AUTO-RESYNC reseed at frame %d seed 0x%04x -> 0x%04x ctr=%u "
                "(sustained desync recovered without a manual retry)",
                s_netFrame, before, (unsigned)Nc_GetInitSeed(), (unsigned)*ADDR_RNG_CTR);
        }
        /* de-merged per-player words for the menu FSM (in menus the local poll is
         * the menu-bit layout, so these are P1's / P2's raw menu words). */
        Nc_GetLastSplit(&s_netP1Menu, &s_netP2Menu);
        *ADDR_INPUT_MENU = merged;          /* menus together; in-stage -> g_InputGameplay */
        /* menus only: make hold-to-scroll deterministic by recomputing the key-repeat
         * from the merged word (ZUN derived it from the local poll above) — fixes the
         * peers landing on different difficulties. */
        if (!inStage) SyncMenuRepeat(merged);

        /* live telemetry for the overlay + log: the RNG pair we compare and how long
         * this frame blocked on the peer (the direct read on a lockstep stall). */
        int connected = Nc_IsConnected();
        int sync = connected ? Nc_IsSync() : 1;
        Nc_GetSyncStats(&s_netSelfRng, &s_netRcvRng, &s_netWaitMs);
        s_netSync = sync;

        /* §8af: NETPLAY STARTUP SYNC — pin the FRONT-END RNG seed to the shared init
         * seed every menu frame. ZUN seeds the live RNG from timeGetTime() at title
         * init (FUN_00438986, PCBdecomp:23325), so each machine's menu RNG evolves
         * from a different base. The once-per-scene SEEDFORCE (HookedGameStart) only
         * realigns the seed AT stage entry, leaving a window where ZUN's stage setup
         * can draw from the per-machine pre-force state — the nondeterministic
         * "syncs after a random number of restarts" startup desync. Pinning the seed
         * to initSeed across the front end hands stage 1 an identical RNG state on
         * both machines, closing that window; the per-scene force then stays as a
         * belt-and-suspenders. IN-STAGE is deliberately untouched (lockstep keeps the
         * naturally-evolving RNG aligned; pinning there caused the old Chen's-orbs
         * bug). Runs BEFORE ZUN's menu update tasks (those fire after this hook
         * returns; s_origSceneTick above only polled input). Guarded off replay
         * playback so the §8ae replay-seed restore path is never disturbed, and off a
         * 0 seed (link not fully up yet). */
        if (s_netActive && !inStage && !IsReplayPlayback()) {
            unsigned short shared = (unsigned short)Nc_GetInitSeed();
            if (shared) *ADDR_RNG_SEED = shared;
        }

        NetTrace(s_netFrame, inStage, merged, s_netWaitMs, sync);  /* DIAGNOSTIC */

        /* DIAGNOSTIC: surface the send-side fault detectors the moment they fire.
         * selfRewrites>0 => a logic frame's input was recorded twice with different
         * values (the merged word feeding our own buffer is mutating it). sendZfill>0
         * => SendKeys transmitted a 0 for a recent frame that should have existed.
         * Either pins the guest->host drop (§8l) to its mechanism. */
        {
            static int s_lastRw = 0, s_lastZf = 0;
            int rw=0, rwf=-1, sz=0, zff=-1, zfs=-1; unsigned short rwo=0, rwn=0;
            Nc_GetSendDiag(&rw, &rwf, &rwo, &rwn, &sz, &zff, &zfs);
            if (rw > s_lastRw) {
                Log("netplay: SELF-REWRITE #%d frame=%d old=0x%04x new=0x%04x (netF=%d)",
                    rw, rwf, rwo, rwn, s_netFrame);
                s_lastRw = rw;
            }
            if (sz > s_lastZf) {
                Log("netplay: SEND-ZFILL #%d sendframe=%d missingslot=%d (netF=%d)",
                    sz, zff, zfs, s_netFrame);
                s_lastZf = sz;
            }
        }

        /* DESYNC is only meaningful IN-STAGE. The front-end menus are not RNG-locked
         * (the two machines reach a screen from slightly different states), so a seed
         * mismatch there is expected — reporting it as a "desync" was the misleading
         * frame-4 flapping. Only judge sync while a stage is actually running, and
         * require a sustained run before declaring recovery (no more lying resyncs). */
        if (inStage && connected) {
            if (!sync) {
                s_netSyncRun = 0;
                if (!s_netDesyncLogged) {
                    Log("netplay: DESYNC in stage at frame %d (rng self=0x%04x peer=0x%04x ctr=%u)",
                        s_netFrame, s_netSelfRng, s_netRcvRng, (unsigned)*ADDR_RNG_CTR);
                    s_netDesyncLogged = 1;
                }
                /* §8r: a blip (≤15 frames) heals at the next scene/seed-reforce; a SUSTAINED
                 * run is the real first-run fork. Flag it distinctly, ONCE, with the local
                 * RNG counter (diff host-vs-guest logs: if the counters differ at the fork,
                 * one machine drew a different number of times => an FP-codepath divergence,
                 * not an input divergence) and the live x87 control word (confirm it was
                 * pinned to 24-bit at the fork — ruling FPU precision in or out). */
                if (++s_netDesyncRun == HARD_DESYNC_FRAMES && !s_netHardDesyncLogged) {
                    Log("netplay: *** HARD DESYNC *** sustained %d frames (real fork) at frame %d "
                        "rng self=0x%04x peer=0x%04x localCtr=%u cw=0x%04x pinned=%d — retry "
                        "(Escape->Give up->Retry) re-aligns via scene-reset+SEEDFORCE.",
                        HARD_DESYNC_FRAMES, s_netFrame, s_netSelfRng, s_netRcvRng,
                        (unsigned)*ADDR_RNG_CTR, _controlfp(0, 0) & 0xffff, s_fpuPinned);
                    s_netHardDesyncLogged = 1;
                }
            } else {
                s_netDesyncRun = 0;
                if (s_netDesyncLogged && ++s_netSyncRun >= 120) {
                    Log("netplay: back in sync at frame %d (held 120 frames)", s_netFrame);
                    s_netDesyncLogged = 0;
                    s_netHardDesyncLogged = 0;
                    s_netSyncRun = 0;
                }
            }
        }

        /* STALL warning: the lockstep blocked noticeably this frame. At a stage load
         * this can spike legitimately; a sustained spike is the peer falling behind or
         * the 5s timeout approaching. Throttle to ~1/sec so the log stays readable. */
        if (connected && s_netWaitMs >= 250 && --s_netStallLogged <= 0) {
            Log("netplay: STALL — waited %dms for the peer at frame %d "
                "(stage-load hitch, or the peer fell behind)", s_netWaitMs, s_netFrame);
            s_netStallLogged = 60;
        }
        /* periodic heartbeat so the log carries a sync timeline even outside stages. */
        if (connected && --s_netStatLogged <= 0) {
            Log("netplay: F%d %s wait=%dms rng self=0x%04x peer=0x%04x ctr=%u%s",
                s_netFrame, sync ? "SYNC" : "DESYNC", s_netWaitMs,
                s_netSelfRng, s_netRcvRng, (unsigned)*ADDR_RNG_CTR,
                inStage ? " [stage]" : " [menu]");
            s_netStatLogged = 120;
        }

        if (!connected && s_netActive) {
            Log("netplay: peer lost (lockstep timeout) — frame %d. Dropping to "
                "local-keyboard P2.", s_netFrame);
            s_netPeerLost = 1;
            s_netActive = 0;               /* drop to local-keyboard P2 fallback */
        }
    }
    return r;
}

/* FUN_00442c60 — game/stage-start init. Force the shared seed before ZUN
 * snapshots it (idempotent on the documented mid-stage re-fires). */
static int __fastcall HookedGameStart(int self)
{
    /* FUN_00442c60 turned out to fire EVERY logic frame (the det-trace showed
     * ~15k SEEDFORCE lines over ~15k frames), NOT just at game start — so the old
     * "force the seed on every fire" pinned the live RNG to initSeed every frame.
     * That (a) makes RNG non-random across frames (Chen's nonspell orbs all flew
     * the same way) and (b) masked real desyncs (both snap to initSeed => false
     * SYNC). Force ONCE per scene instead (EoSD's force-at-start semantics): align
     * the seed at stage start, then let ZUN's RNG evolve naturally — lockstep keeps
     * both machines identical as long as inputs match. s_seedForced re-arms on each
     * scene change (HookedSceneTick). */
    if (s_netActive && !s_seedForced) {
        uint16_t before = *ADDR_RNG_SEED;
        *ADDR_RNG_SEED = Nc_GetInitSeed();
        s_seedForced = 1;
        /* §8ag: the game-start difficulty FORCE that used to live here (§8v) is gone — it
         * patched the difficulty-derived run snapshot AFTER the guest had already confirmed
         * the wrong difficulty, which was fragile and incomplete. Difficulty is now correct
         * before this point because both peers selected it from the same reset cursor
         * (link-up §8ag), so ZUN's own snapshot already latches the matching value. The
         * SEEDFORCE diff= below is the audit: host and guest must print the same diff=. */
        /* §8r: log the x87 control word at the seeding moment too — if host and guest
         * differ here on the first stage, FP precision forked before the per-frame pin
         * settled; if they match (both 0x_01f) the first-run fork is elsewhere.
         * diff= lets the tester confirm both sides entered the stage on the SAME value. */
        Log("netplay: SEEDFORCE F%d seed 0x%04x -> 0x%04x ctr=%u cw=0x%04x diff=%u (once/scene)",
            s_netFrame, before, (unsigned)Nc_GetInitSeed(), (unsigned)*ADDR_RNG_CTR,
            _controlfp(0, 0) & 0xffff, (unsigned)*ADDR_DIFFICULTY);
    }
    return s_origGameStart(self);
}

/* §8ag: force Extra/Phantasm "unlocked" under netplay so the guest's menu offers the same
 * options as the host's regardless of each install's score.dat (otherwise the guest can't
 * follow the host into Extra/Phantasm). Off netplay, vanilla behaviour. Returning 1 early
 * also skips FUN_0042f94c's score-data self-write — fine, we don't mutate the save. */
static int __fastcall HookedExtraUnlocked(int self)
{
    if (s_netActive) return 1;
    return s_origExtraUnlocked(self);
}
static int __fastcall HookedPhantUnlocked(int self)
{
    if (s_netActive) return 1;
    return s_origPhantUnlocked(self);
}
/* ===================== end netplay ===================== */

/* ---- co-op replay tagging (FUN_00443040 — replay header builder) -------------
 * PCB already records the full 16-bit input word per frame (FUN_00442cd0 saves
 * g_InputMenu), and our merge puts P2 in the high 7 bits — netplay merges in
 * HookedSceneTick, LOCAL co-op merges in HookedFrameTask right before the record —
 * so a co-op .rpy carries P2's whole input stream. The one datum the vanilla header lacks is WHICH
 * character P2 picked. We stash it (so co-op runs save as complete replays now;
 * actual two-player PLAYBACK is a planned follow-up that will read this back).
 *
 * Safe by construction: the loader (FUN_004433b0 validates only magic@0x00 +
 * the @0x08 checksum; FUN_00443550 consumes only 0x56/0x57, the 0x14-0x53 stage
 * table, and 0x70-0xa7) never touches the 0x58-0x5d gap, and the header builder
 * never writes it. We drop a 4-byte magic-tagged block there; the @0x08 checksum
 * is computed by the save AFTER this runs, so the file stays self-consistent and
 * vanilla PCB still loads it. Only written when co-op is actually in play, so a
 * solo run made with the DLL stays a byte-clean vanilla replay. */
#define ADDR_REPLAY_HDR_INIT ((LPVOID)0x00443040)
/* Header-gap tag (0x58-0x5d): in-memory "watch replay" ONLY. A SAVED .rpy overwrites it with
 * ZUN's date (0x58) and player name (0x5e) — the durable co-op block lives in the stage block
 * (§8ah: seed at +0x20, magic/p2sel/flags at +0x28-0x2b). */
#define RPY_COOP_OFF  0x58                 /* header gap 0x58-0x5d (in-memory playback only)      */
#define RPY_COOP_MAG0 0xC2                 /* co-op block magic / format tag                      */
#define RPY_COOP_MAG1 0x07
#define RPY_CW_OFF    0x5d                 /* §8ad: recording's x87 cw nibble (0x80|((cw>>8)&0xf)) */
typedef uint32_t (__fastcall *ReplayHdrFn_t)(void *self);
static ReplayHdrFn_t s_origReplayHdr = NULL;
static int s_rpyTagLogged = 0;
static unsigned char *s_rpyHdrBuf = NULL;  /* §8ad: live header buffer, so the SIM can stamp the cw */
static int s_rpySeedLogged = 0;            /* §8ah: log the stage-block seed fix once per recording */
static unsigned char *s_rpyStageBlk = NULL;/* §8ah: current recording stage block (seed backfill target) */

static uint32_t __fastcall HookedReplayHdrInit(void *self)
{
    uint32_t r = s_origReplayHdr(self);
    /* param_1[1] is the 0xe8-byte header buffer (allocated on the first call). */
    unsigned char *hdr = (unsigned char *)((void **)self)[1];
    /* Tag whenever P2 is part of this run. The old gate ((s_p2Sel>=0)||s_netActive)
     * MISSED a LOCAL co-op replay: there P2 auto-spawns mirroring P1, so s_p2Sel stays
     * -1 and s_netActive is 0 — the header went untagged, so playback couldn't detect
     * the co-op replay and never spawned P2 (→ replay desync once aimed enemies appear).
     * With the co-op DLL loaded P2 ALWAYS auto-spawns unless suppress_p2 pins it off, so
     * "this is a co-op run" is simply !s_suppressP2 — and that's TIMING-INDEPENDENT
     * (FUN_00443040 is a task callback that may fire at record START, before P2 has
     * spawned, so s_autoSpawned/s_p2 can't be relied on here). suppress_p2=1 keeps a
     * P2-less run a byte-clean vanilla replay. */
    int coop = !s_suppressP2;
    if (hdr && coop) {
        int p2sel = (s_p2Sel >= 0) ? s_p2Sel : (int)*ADDR_SEL_ID;  /* mirror -> P1 */
        unsigned char netFlag = (unsigned char)(s_netActive ? 0x01 : 0x00);
        /* Header-gap tag (0x58-0x5d): recovered by playback ONLY for the in-memory "watch replay"
         * path. A SAVED .rpy clobbers it — ZUN's results screen writes the replay DATE string at
         * 0x58 and the player NAME at 0x5e (a reloaded header reads "06" at 0x58, not our magic —
         * confirmed in the log). So the DURABLE co-op block + seed live in the per-stage block
         * below; this header tag is kept only as a best-effort for immediate post-run playback. */
        hdr[RPY_COOP_OFF + 0] = RPY_COOP_MAG0;
        hdr[RPY_COOP_OFF + 1] = RPY_COOP_MAG1;
        hdr[RPY_COOP_OFF + 2] = (unsigned char)p2sel;
        hdr[RPY_COOP_OFF + 3] = (unsigned char)(s_allowDiffChar ? 1 : 0);
        /* §8aj: bit1 = "per-frame seed present" (HookedFrameTask stores the frame-start seed in each
         * entry's unused flags half for net co-op recordings — see HookedFrameTask / HookedPlayTask). */
        hdr[RPY_COOP_OFF + 4] = (unsigned char)(netFlag | (s_netActive ? 0x02 : 0x00));
        hdr[RPY_CW_OFF]       = 0x00;
        s_rpyHdrBuf = hdr;
        /* §8ah: durable co-op metadata in the per-stage block (round-trips through a saved .rpy,
         * unlike the header gap). FUN_00443040 just allocated this stage's block at hdr+0x1c+stage*4
         * and set its +0x20 seed slot to DAT_0062f854 (the PRE-force seed ZUN snapshots BEFORE the
         * netcode's per-scene SEEDFORCE) — overwrite it with the FORCED init seed so ZUN's own restore
         * (FUN_00443550: DAT_0049fe20 = stageblock+0x20) reproduces the RNG state the live sim actually
         * ran from. This was THE netplay-replay desync, and the fix now lands on BOTH the tagged and
         * the force_replay_p2 playback paths. The 0x28-0x2b gap (between the last block field at +0x27
         * and the input stream at +0x2c) carries the co-op tag so playback auto-detects P2 from a
         * saved replay. Local/solo runs never force-seed, so they leave +0x20 as ZUN set it. */
        s_rpyStageBlk = NULL;
        {
            int st = (int)*(volatile uint32_t *)0x0062f85c - 1;
            if (st < 0) st = 0;
            if (st > 6) st = 6;
            unsigned char *sb = *(unsigned char **)(hdr + 0x1c + st * 4);
            if (sb) {
                s_rpyStageBlk = sb;                      /* backfill target if the link came up late */
                if (s_netActive) {
                    unsigned short is = (unsigned short)Nc_GetInitSeed();
                    if (is) {                            /* skip a 0 seed (link not up yet) */
                        unsigned short b4 = *(unsigned short *)(sb + 0x20);
                        *(unsigned short *)(sb + 0x20) = is;
                        if (!s_rpySeedLogged) {
                            Log("replay: stage-seed fix sb+0x20 0x%04x -> 0x%04x (stage=%d) "
                                "[§8ah: ZUN-native seed slot, survives saved .rpy]", b4, is, st + 1);
                            s_rpySeedLogged = 1;
                        }
                    }
                }
                sb[0x28] = RPY_COOP_MAG0;
                sb[0x29] = (unsigned char)p2sel;
                /* bit0 diff-char, bit1 net-recorded, bit2 = §8aj per-frame seed present (the durable,
                 * saved-replay copy of the hdr+0x5c bit1 above). */
                sb[0x2a] = (unsigned char)((s_allowDiffChar ? 1 : 0) | (s_netActive ? 2 : 0)
                                           | (s_netActive ? 4 : 0));
                sb[0x2b] = 0;
            }
        }
        if (!s_rpyTagLogged) {
            int n = (p2sel >= 0 && p2sel <= 5) ? p2sel : 0;
            Log("replay: tagged co-op header — P2=%s diff=%d (hdr +0x%02x / stageblock +0x28)",
                ADDR_SEL_NAMES[n], s_allowDiffChar, RPY_COOP_OFF);
            s_rpyTagLogged = 1;
        }
    }
    return r;
}

/* ---- co-op replay PLAYBACK (§8k) ----------------------------------------------
 * The record side (above) already tags the header (0x58 block) and the merged input
 * word carries P2 in its high bits, so a co-op .rpy is complete. Playback needs three
 * things, all wired here: (1) recover P2's character from the header, (2) auto-spawn the
 * P2 clone, (3) source P2's input from the REPLAYED word instead of the keyboard/wire.
 * PCB selects record vs playback by which task it registers (FUN_00442cd0 record /
 * FUN_00442ee0 playback, PCBdecomp 27968-28003); the chosen mode is stored at
 * replayMgr+0x44 (0=record, 1=playback). In playback FUN_00442ee0 writes the recorded
 * 16-bit word into g_InputGameplay, so its HIGH bits ARE P2's recorded input. */
#define REPLAY_MODE_OFF  0x44                     /* mgr+0x44: 0 record, 1 playback         */
#define ADDR_REPLAY_LOAD ((LPVOID)0x00443550)     /* FUN_00443550 — playback header consume */
#define ADDR_REPLAY_PLAY_TASK ((LPVOID)0x00442ee0) /* FUN_00442ee0 — per-frame PLAYBACK input task */
typedef uint32_t (__fastcall *ReplayLoadFn_t)(void *self);
static ReplayLoadFn_t s_origReplayLoad = NULL;
typedef int (__fastcall *PlayTaskFn_t)(int *self);
static PlayTaskFn_t s_origPlayTask = NULL;
/* s_replayIsCoop + s_replayNetRecorded are declared earlier (before HookedSceneTick). */
static int s_replayP2Sel    = -1;  /* P2's character recovered from the header             */
static int s_replayDiffChar = 0;   /* P2 was a different char (header byte 0x5b)           */
static int s_replayLoadLogged = 0;
/* coop.ini [coop] force_replay_p2: -1 = off (default). 0..5 = treat an UNTAGGED replay as
 * co-op and spawn P2 with this selection (ReimuA..SakuyaB). For replays recorded before the
 * tag fix: P2's per-frame input is ALREADY in the file (merge high bits), only the spawn tag
 * was missing. Use the SAME character family as P1 for a stable, accurate playback — a
 * different-char P2 is the unstable anm path (can crash on shoot). s_forceReplayP2 is
 * declared up with the other [coop] config statics (it's read in LoadNetConfig). */
#define RPY_P1CHAR_OFF 0x56        /* header byte 0x56 = P1's character (DAT_0062f647)     */

static int IsReplayPlayback(void)
{
    void *mgr = *ADDR_REPLAY_MGR;
    return mgr && *(int *)((char *)mgr + REPLAY_MODE_OFF) == 1;
}

/* FUN_00443550 — playback header-consume task: after it loads the header into self[1],
 * read our 0x58 co-op block back to recover P2's character. A solo/vanilla replay has no
 * block -> s_replayIsCoop=0 so we never spawn a phantom P2 over a one-player replay. */
static uint32_t __fastcall HookedReplayLoad(void *self)
{
    uint32_t r = s_origReplayLoad(self);
    unsigned char *hdr = self ? (unsigned char *)((void **)self)[1] : NULL;
    /* §8ah: the DURABLE co-op tag lives in the current stage block (+0x28), which round-trips
     * through a saved .rpy; the header 0x58 gap only survives in-memory playback (ZUN's date/name
     * fields clobber it on save). s_origReplayLoad just made the +0x1c stage pointers absolute, so
     * read the block for the stage being loaded (DAT_0062f85c). The RNG seed is restored natively by
     * ZUN from stageblock+0x20 (which we forced at record), so playback needs no seed re-force. */
    unsigned char *sb = NULL;
    if (hdr) {
        int st = (int)*(volatile uint32_t *)0x0062f85c - 1;
        if (st < 0) st = 0;
        if (st > 6) st = 6;
        sb = *(unsigned char **)(hdr + 0x1c + st * 4);
    }
    int sbTag  = (sb && sb[0x28] == RPY_COOP_MAG0);
    int hdrTag = (hdr && hdr[RPY_COOP_OFF] == RPY_COOP_MAG0 && hdr[RPY_COOP_OFF + 1] == RPY_COOP_MAG1);
    if (sbTag || hdrTag) {
        s_replayIsCoop = 1;
        if (sbTag) {                                        /* saved-replay path (durable) */
            s_replayP2Sel       = (signed char)sb[0x29];
            s_replayDiffChar    = (sb[0x2a] & 1) ? 1 : 0;
            s_replayNetRecorded = (sb[0x2a] & 2) ? 1 : 0;
            s_replayPerFrameSeed = (sb[0x2a] & 4) ? 1 : 0;  /* §8aj per-frame seed in each entry */
        } else {                                            /* in-memory watch-replay path */
            s_replayP2Sel       = (signed char)hdr[RPY_COOP_OFF + 2];
            s_replayDiffChar    = hdr[RPY_COOP_OFF + 3] ? 1 : 0;
            s_replayNetRecorded = (hdr[RPY_COOP_OFF + 4] & 0x01) ? 1 : 0;   /* §8aa */
            s_replayPerFrameSeed = (hdr[RPY_COOP_OFF + 4] & 0x02) ? 1 : 0;  /* §8aj */
        }
        s_replayTargetCw = 0x007f;   /* sim is 24-bit in both net & local; pin neutered (§8ad refuted) */
        if (!s_replayLoadLogged) {
            int n = (s_replayP2Sel >= 0 && s_replayP2Sel <= 5) ? s_replayP2Sel : 0;
            Log("replay PLAYBACK: co-op replay — P2=%s diffchar=%d netRec=%d src=%s "
                "(stageblock +0x28 / header +0x%02x)", ADDR_SEL_NAMES[n], s_replayDiffChar,
                s_replayNetRecorded, sbTag ? "stageblock" : "header", RPY_COOP_OFF);
            s_replayLoadLogged = 1;
        }
    } else if (IsReplayPlayback() && s_forceReplayP2 >= 0 && hdr) {
        /* untagged replay + user override: force co-op playback. P2's input is already in
         * the file's high bits; we just spawn P2. diffchar = forced char's family differs
         * from P1's (header 0x56) — a different family uses the unstable anm overlay. */
        s_replayIsCoop   = 1;
        s_replayP2Sel    = s_forceReplayP2;
        s_replayDiffChar = ((s_forceReplayP2 / 2) != (int)hdr[RPY_P1CHAR_OFF]) ? 1 : 0;
        s_replayNetRecorded = 0;   /* §8aa: untagged/forced — flag unknown */
        s_replayPerFrameSeed = 0;  /* §8aj: untagged replay has no per-frame seed stream */
        s_replayTargetCw = 0x007f; /* §8ad: sim runs at 24-bit either way */
        if (!s_replayLoadLogged) {
            int n = (s_forceReplayP2 <= 5) ? s_forceReplayP2 : 0;
            Log("replay PLAYBACK: FORCED co-op (force_replay_p2=%d -> P2=%s diffchar=%d, "
                "P1char=%d) — untagged replay (seed still fixed via ZUN's stageblock+0x20)",
                s_forceReplayP2, ADDR_SEL_NAMES[n], s_replayDiffChar, (int)hdr[RPY_P1CHAR_OFF]);
            s_replayLoadLogged = 1;
        }
    } else {
        s_replayIsCoop = 0;   /* solo/vanilla replay — no phantom P2 */
        s_replayNetRecorded = 0;
        s_replayPerFrameSeed = 0;  /* §8aj */
        s_replayTargetCw = 0x007f;
        if (IsReplayPlayback() && !s_replayLoadLogged) {  /* why a replay didn't recover P2 */
            Log("replay PLAYBACK: NO co-op tag (stageblock+0x28=%02x header+0x%02x=%02x %02x) — "
                "solo/vanilla or recorded pre-fix (set [coop] force_replay_p2 to spawn P2 anyway)",
                sb ? sb[0x28] : 0xFF, RPY_COOP_OFF, hdr ? hdr[RPY_COOP_OFF] : 0xFF,
                hdr ? hdr[RPY_COOP_OFF + 1] : 0xFF);
            s_replayLoadLogged = 1;
        }
    }
    /* §8ad: decide the playback FPU pin ONCE here (HookedReplayLoad always runs on load),
     * because HookedSceneTick's pin block does NOT execute during in-stage replay playback
     * (the replay drives a different task path — confirmed: no "fpu-pin decision" line, pin=0).
     * The actual fldcw is applied each frame by HookedPlayTask (frame-start) + the sim seam. */
    {
        unsigned short tgt = s_replayTargetCw;   /* = the cw the SIM recorded at (24-bit for both net & local) */
        int pin = (s_replayFpuPin == 0) ? 0
                : (s_replayFpuPin == 1) ? 1
                : (s_replayIsCoop && tgt != 0x007f);              /* auto (-1): only if a non-24-bit cw was stamped */
        s_replayPinCw = pin ? tgt : 0;
        if (IsReplayPlayback())
            Log("replay PLAYBACK: fpu-pin decision=%s targetCw=0x%04x (replay_fpu_pin=%d netRec=%d "
                "coop=%d) — reproducing the recording's x87 precision (§8ad).",
                pin ? "PIN" : "native", tgt, s_replayFpuPin, s_replayNetRecorded, s_replayIsCoop);
    }
    /* §8ah: no RNG seed re-force here — ZUN's own restore (s_origReplayLoad, FUN_00443550:
     * DAT_0049fe20 = stageblock+0x20) already loaded the FORCED seed we wrote into that slot at
     * record time. Log it (always-on, one line per stage load) so a normal coop_log.txt confirms
     * the seed round-trip without needing the replay_trace CSV: if seed==stageblock+0x20==initSeed
     * the start state is correct and any residual desync is a rand-CALL-COUNT divergence, not the
     * seed. (RNG output depends only on DAT_0049fe20; DAT_0049fe24 is a pure call counter.) */
    if (IsReplayPlayback() && sb) {
        Log("replay PLAYBACK: stage=%d seed=0x%04x (stageblock+0x20=0x%04x tag+0x28=0x%02x) "
            "[§8ah seed round-trip check]", (int)*(volatile uint32_t *)0x0062f85c,
            (unsigned)*ADDR_RNG_SEED, (unsigned)*(unsigned short *)(sb + 0x20), sb[0x28]);
    }
    /* §8ai: arm a seed re-force for the FIRST IN-STAGE playback frame. ZUN's stage-load restore
     * (above) lands initSeed too EARLY — before ZUN's stage-setup draws ~80-95 RNG calls. In the
     * LIVE netplay run the per-scene SEEDFORCE fires at the stage's first frame and WIPES those
     * setup draws (the trace shows the live stage starting clean at initSeed, e.g. rec rf0 seed ==
     * initSeed, while playback starts at initSeed-evolved-by-setup, e.g. f7c5) — so without a
     * matching wipe every enemy's RNG forks from the first gameplay frame. Re-apply initSeed at the
     * first in-stage frame (HookedUpdate) to reproduce the live SEEDFORCE's wipe-point. Net-recorded
     * only: local replays never force-seed, so ZUN's setup draws ARE part of their canonical flow. */
    /* §8al: §8aj's per-frame restore runs at FRAME START (HookedPlayTask), but ZUN's ~95 stage-setup
     * draws on playback run AFTER that (during frame 1's sim), so the per-frame restore does NOT wipe
     * them — the trace confirms rf1's seed is the ONLY mismatched frame. §8ai's re-force fires in
     * HookedUpdate, AFTER the setup draws, so it IS the wipe-point. Arm it for ALL net-recorded replays
     * (no longer gated off by §8aj) so frame 1's gameplay seed is correct too; §8aj handles 2..N. */
    if (IsReplayPlayback() && s_replayNetRecorded && sb) {
        s_replayInitSeed = *(unsigned short *)(sb + 0x20);
        s_replaySeedArm  = s_replayInitSeed ? 1 : 0;
    }
    /* §8aj per-frame seed re-pins frames 2..N; §8ai (re-armed above) wipes ZUN's setup draws at the
     * first in-stage frame (which §8aj's frame-start restore is too early to catch). Leave §8ai
     * as the fallback for OLDER net replays whose entries have no stored seed (s_replayPerFrameSeed=0). */
    if (IsReplayPlayback() && sb)
        Log("replay PLAYBACK: §8aj per-frame seed = %s (sb+0x2a=0x%02x) — %s",
            s_replayPerFrameSeed ? "PRESENT" : "absent", sb[0x2a],
            s_replayPerFrameSeed ? "re-pinning seed every frame" : "falling back to §8ai single re-force");
    return r;
}

/* §8ad: FUN_00442ee0 — the per-frame PLAYBACK input task (mirror of the record task FUN_00442cd0).
 * THIS is the frame-start hook that actually runs during in-stage replay playback, so it's where
 * the recorded x87 precision must be re-asserted (before the frame's whole sim — enemies, bullets,
 * players). HookedSceneTick doesn't fire here; HookedReplayLoad decided s_replayPinCw. */
static int __fastcall HookedPlayTask(int *self)
{
    /* §8aj: per-frame seed restore. Mirror of HookedFrameTask's capture. ZUN's playback task
     * FUN_00442ee0 reads the recorded input from the head entry (self+0x84) and advances the head
     * by 4 — it never touches the entry's +2 flags half, where the record build stored the live
     * frame-START seed. Restore it here, AT THE SAME frame-phase (this task runs before the frame's
     * sim), so this frame's enemies/bullets draw from the exact recorded seed. This makes any
     * rand-CALL-COUNT difference between the live run and playback irrelevant: the seed is re-pinned
     * to the recorded value every single frame, not just once at stage start (§8ai). Gated on the
     * §8aj version bit so OLD net replays (no per-frame seed; flags half = 0) are left to §8ai. */
    unsigned char *head0 = s_replayPerFrameSeed ? *(unsigned char **)((char *)self + 0x84) : NULL;
    int r = s_origPlayTask(self);
    if (head0) {
        unsigned char *head1 = *(unsigned char **)((char *)self + 0x84);
        if (head1 == head0 + 4) {                  /* the task consumed a per-frame entry */
            *ADDR_RNG_SEED = *(unsigned short *)(head0 + 2);     /* recorded frame-start seed */
            if (++s_rpySeedRestored == 1)
                Log("replay PLAYBACK: §8aj per-frame seed restore ON — re-pinning the RNG seed to the "
                    "recorded value every frame (supersedes the §8ai single first-frame re-force)");
        }
    }
    if (s_replayPinCw) PinFpuForReplay(s_replayPinCw);
    return r;
}

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

    /* pick the highest free slot the ENGINE never loads into (see kGameAnmSlots):
     * any slot the game itself uses would be freed-then-reloaded out from under
     * our captured pointers when the game wants it (dialogue, boss, ending) -> a
     * "crashes after some time" dangling deref. 0x30 down to 0x14 are the safe
     * candidates. */
    slot = -1;
    for (id = 0x30; id >= 0x02; id--) {
        if (IsGameAnmSlot(id)) continue;
        if (*(uint32_t *)(slt + id * 0xc) == 0) { slot = id; break; }
    }
    if (slot < 0) { Log("P2 char anm: no free non-engine slot"); return 0; }

    /* snapshot P1's tables across the window, then load P2's char OVER base
     * 0x400 (overwrites only the ids P2's anm defines), diff, restore P1's.
     * The reverse-base table is snapshot+restored too: the load's internal
     * slot-free zeroes it, and although the re-register writes back the same
     * base (0x400), restoring P1's exact values keeps us robust. */
    {
        uint32_t p1Script[ANM_MAX_IDS];
        uint32_t p1Rev[ANM_MAX_IDS];
        unsigned char p1Sprite[ANM_MAX_IDS][ANM_SPRITE_STRIDE];
        char *rev = (char *)(mgr + ANM_REV_TBL);
        int i, rc;
        for (i = 0; i < ANM_MAX_IDS; i++) {
            p1Script[i] = *(uint32_t *)(scr + (ANM_ID_LO + i) * 4);
            p1Rev[i]    = *(uint32_t *)(rev + (ANM_ID_LO + i) * 4);
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
                *(uint32_t *)(rev + (ANM_ID_LO + i) * 4) = p1Rev[i];
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
                /* capture P2's reverse-base too (= 0x400, the load base). The
                 * sprite bind computes global id = local + reverse[id]; at
                 * Marisa-only ids P1's reverse is 0, so without swapping this the
                 * laser binds to local+0 (a font glyph) instead of local+0x400. */
                s_p2Rev[n] = *(uint32_t *)(rev + (ANM_ID_LO + i) * 4);
                memcpy(s_p2Sprite[n],
                       spr + (ANM_ID_LO + i) * ANM_SPRITE_STRIDE,
                       ANM_SPRITE_STRIDE);
                n++;
            }
            /* restore P1's live tables (script, reverse-base, sprite) */
            *(uint32_t *)(scr + (ANM_ID_LO + i) * 4) = p1Script[i];
            *(uint32_t *)(rev + (ANM_ID_LO + i) * 4) = p1Rev[i];
            memcpy(spr + (ANM_ID_LO + i) * ANM_SPRITE_STRIDE, p1Sprite[i],
                   ANM_SPRITE_STRIDE);
        }
    }

    s_p2AnmChar  = charId;
    s_p2AnmSlot  = slot;
    s_p2AnmFile  = *(uint32_t *)(slt + slot * 0xc);  /* our file ptr; SwapAnm guards on it */
    s_p2IdCount  = n;
    Log("P2 char anm: %s -> slot %d (file 0x%08x), %d ids captured (0x%x..0x%x)",
        anmNames[charId], slot, s_p2AnmFile, n,
        n ? s_p2Ids[0] : 0, n ? s_p2Ids[n - 1] : 0);
    return n > 0;
}

static void FreeP2CharAnm(void)
{
    uint32_t mgr = *ADDR_ANM_MGR_PP;
    /* MUST be called with the tables in P1's (rest) state — callers ensure it. */
    if (s_p2AnmSlot >= 0 && mgr) {
        /* The engine slot-free FUN_0044e4e0 ZEROES the global script, sprite AND
         * reverse-base tables for this slot's ids (PCBdecomp.c:32925-32936). But
         * P2 loaded at base 0x400, so its ids are the SAME ones P1 uses, and the
         * tables right now hold P1's live entries — a naive free wipes P1's
         * body/shot scripts AND the reverse-base entries the sprite-bind reads
         * (P1 vanishes / renders as glyphs, then a bad bind crashes). So snapshot
         * P1's window (all three tables), let the engine free (which also
         * releases P2's buffer + textures), then restore P1's. */
        char *scr = (char *)(mgr + ANM_SCRIPT_TBL);
        char *rev = (char *)(mgr + ANM_REV_TBL);
        char *spr = (char *)(mgr + ANM_SPRITE_TBL);
        uint32_t p1Script[ANM_MAX_IDS];
        uint32_t p1Rev[ANM_MAX_IDS];
        static unsigned char p1Sprite[ANM_MAX_IDS][ANM_SPRITE_STRIDE];
        int i;
        for (i = 0; i < ANM_MAX_IDS; i++) {
            p1Script[i] = *(uint32_t *)(scr + (ANM_ID_LO + i) * 4);
            p1Rev[i]    = *(uint32_t *)(rev + (ANM_ID_LO + i) * 4);
            memcpy(p1Sprite[i], spr + (ANM_ID_LO + i) * ANM_SPRITE_STRIDE,
                   ANM_SPRITE_STRIDE);
        }
        ADDR_ANM_FREE_FN((void *)mgr, s_p2AnmSlot);
        for (i = 0; i < ANM_MAX_IDS; i++) {
            *(uint32_t *)(scr + (ANM_ID_LO + i) * 4) = p1Script[i];
            *(uint32_t *)(rev + (ANM_ID_LO + i) * 4) = p1Rev[i];
            memcpy(spr + (ANM_ID_LO + i) * ANM_SPRITE_STRIDE, p1Sprite[i],
                   ANM_SPRITE_STRIDE);
        }
        Log("P2 char anm: freed slot %d (P1 window restored)", s_p2AnmSlot);
    }
    s_p2AnmChar = -1;
    s_p2AnmSlot = -1;
    s_p2AnmFile = 0;
    s_p2IdCount = 0;
    s_p2AnmActive = 0;
    s_anmSwapped = 0;
}

/* ---- §8b DIAGNOSTIC: instrumented P2 face-anm load (default off) ----
 * The open §8b goal is the CORRECT bomb-declaration face for a different-char P2.
 * Two prior blind attempts loaded data/face_{rm,mr,sk}00.anm and corrupted the
 * 0x400 player table (P1 -> glyph sprites, P2 invisible, crash on fire) even though
 * the face id window [0x4a0,0x4ad) is disjoint from the player's [0x400,0x4a0).
 * Static analysis of the loader (FUN_0044df90) shows a base-0x4a0 load can only
 * write RISING ids/slots, so it cannot touch 0x400 directly — the corruption must
 * arrive through the slot FREE's recursive zero + global sprite-cache reset
 * (FUN_0044e4e0, mgr+0x2e4cc/0x2e4d0..2). The handoff (§8b) is explicit: do NOT
 * retry the swap blind; first INSTRUMENT the load to catch who writes 0x400.
 *
 * This does exactly that and nothing else. Behind coop.ini [coop] p2_face_diag=1,
 * once per P2 character, at the clean stage-start point (right after the proven
 * char-anm load, tables in P1's rest state), it loads P2's face into a spare
 * non-engine slot at base 0x4a0 and logs the three §8b data points:
 *   (1) the slot's consumed chain span (does the face chain past [0x4a0,0x4ad)?),
 *   (2) the player window 0x400.. script/rev/sprite BEFORE vs AFTER the load — the
 *       smoking gun for the corruption, and whether it's the load or only the free,
 *   (3) the actual id span the face anm defines.
 * It then FULLY restores every touched table and frees the slot — a pure read-only
 * probe that leaves engine state pristine, so the shipped suppression baseline is
 * unaffected whether this flag is on or off. Once the log pins the 0x400 writer,
 * the real correct-face swap (LoadP2FaceAnm + SwapFace, mirroring LoadP2CharAnm/
 * SwapAnm) can be built against a known cause instead of guessing. */
#define FACE_ID_LO    0x4a0
#define FACE_ID_HI    0x4e0           /* > P1's [0x4a0,0x4ad): catches any chaining  */
#define FACE_MAX_IDS  (FACE_ID_HI - FACE_ID_LO)
#define PLY_ID_LO     0x400           /* the window §8b reports getting corrupted    */
#define PLY_ID_HI     0x410
#define PLY_MAX_IDS   (PLY_ID_HI - PLY_ID_LO)

static int s_p2FaceDiagDone = -1;     /* charId already probed this game; -1 = none  */

static void LoadP2FaceDiag(int charId)
{
    static const char *const faceNames[3] =
        { "data/face_rm00.anm", "data/face_mr00.anm", "data/face_sk00.anm" };
    uint32_t mgr = *ADDR_ANM_MGR_PP;
    char *scr, *spr, *rev, *slt;
    int slot, i, rc;
    uint32_t fScript[FACE_MAX_IDS], fRev[FACE_MAX_IDS];
    uint32_t pScriptB[PLY_MAX_IDS], pRevB[PLY_MAX_IDS];
    static unsigned char pSprB[PLY_MAX_IDS][ANM_SPRITE_STRIDE];
    static unsigned char fSprS[FACE_MAX_IDS][ANM_SPRITE_STRIDE];

    if (charId < 0 || charId > 2 || !mgr) return;

    scr = (char *)(mgr + ANM_SCRIPT_TBL);
    spr = (char *)(mgr + ANM_SPRITE_TBL);
    rev = (char *)(mgr + ANM_REV_TBL);
    slt = (char *)(mgr + ANM_SLOT_TBL);

    /* a free slot the ENGINE never loads into (the face's own engine slot is 0x19;
     * reusing any engine slot would be yanked out from under us — see kGameAnmSlots) */
    slot = -1;
    for (i = 0x30; i >= 0x02; i--) {
        if (IsGameAnmSlot(i)) continue;
        if (*(uint32_t *)(slt + i * 0xc) == 0) { slot = i; break; }
    }
    if (slot < 0) { Log("[face-diag] no free non-engine slot"); return; }

    /* (2-before) snapshot the player window + the whole face window */
    for (i = 0; i < PLY_MAX_IDS; i++) {
        pScriptB[i] = *(uint32_t *)(scr + (PLY_ID_LO + i) * 4);
        pRevB[i]    = *(uint32_t *)(rev + (PLY_ID_LO + i) * 4);
        memcpy(pSprB[i], spr + (PLY_ID_LO + i) * ANM_SPRITE_STRIDE, ANM_SPRITE_STRIDE);
    }
    for (i = 0; i < FACE_MAX_IDS; i++) {
        fScript[i] = *(uint32_t *)(scr + (FACE_ID_LO + i) * 4);
        fRev[i]    = *(uint32_t *)(rev + (FACE_ID_LO + i) * 4);
        memcpy(fSprS[i], spr + (FACE_ID_LO + i) * ANM_SPRITE_STRIDE, ANM_SPRITE_STRIDE);
    }

    rc = ADDR_ANM_LOAD_FN((void *)mgr, slot, faceNames[charId], FACE_ID_LO);

    /* (1) consumed-chain span for our slot vs the engine's own face slot 0x19
     * (the per-slot span the loader writes at mgr+0x2def8+slot*0xc, = ANM_SLOT_TBL+8) */
    Log("[face-diag] %s -> slot %d rc=%d  chain-span=%u (engine slot0x19 span=%u)",
        faceNames[charId], slot, rc,
        *(uint32_t *)(slt + slot * 0xc + 8),
        *(uint32_t *)(slt + 0x19 * 0xc + 8));

    /* (2-after) re-read the player window — report exactly who/what the load moved */
    {
        int changed = 0;
        for (i = 0; i < PLY_MAX_IDS; i++) {
            uint32_t curS = *(uint32_t *)(scr + (PLY_ID_LO + i) * 4);
            uint32_t curR = *(uint32_t *)(rev + (PLY_ID_LO + i) * 4);
            int sprChg = memcmp(spr + (PLY_ID_LO + i) * ANM_SPRITE_STRIDE,
                                pSprB[i], ANM_SPRITE_STRIDE) != 0;
            if (curS != pScriptB[i] || curR != pRevB[i] || sprChg) {
                Log("[face-diag] !! PLAYER id 0x%x CHANGED by face LOAD: "
                    "script %08x->%08x  rev %08x->%08x  spr %s",
                    PLY_ID_LO + i, pScriptB[i], curS, pRevB[i], curR,
                    sprChg ? "DIFF" : "same");
                changed++;
            }
        }
        if (!changed)
            Log("[face-diag] player window 0x%x..0x%x UNCHANGED by the LOAD "
                "(=> corruption, if any, is the slot FREE)", PLY_ID_LO, PLY_ID_HI - 1);
    }

    /* (3) the actual id span the face anm defines (diff vs the pre-load snapshot) */
    {
        int lo = -1, hi = -1, n = 0;
        for (i = 0; i < FACE_MAX_IDS; i++) {
            uint32_t curS = *(uint32_t *)(scr + (FACE_ID_LO + i) * 4);
            int sprChg = memcmp(spr + (FACE_ID_LO + i) * ANM_SPRITE_STRIDE,
                                fSprS[i], ANM_SPRITE_STRIDE) != 0;
            if (curS != fScript[i] || sprChg) {
                if (lo < 0) lo = FACE_ID_LO + i;
                hi = FACE_ID_LO + i;
                n++;
            }
        }
        Log("[face-diag] face defines %d ids in [0x%x..0x%x] (P1 face uses 0x4a0..0x4ac)",
            n, lo < 0 ? 0 : lo, hi < 0 ? 0 : hi);
    }

    /* restore the face + player windows (memory) ... */
    for (i = 0; i < FACE_MAX_IDS; i++) {
        *(uint32_t *)(scr + (FACE_ID_LO + i) * 4) = fScript[i];
        *(uint32_t *)(rev + (FACE_ID_LO + i) * 4) = fRev[i];
        memcpy(spr + (FACE_ID_LO + i) * ANM_SPRITE_STRIDE, fSprS[i], ANM_SPRITE_STRIDE);
    }
    for (i = 0; i < PLY_MAX_IDS; i++) {
        *(uint32_t *)(scr + (PLY_ID_LO + i) * 4) = pScriptB[i];
        *(uint32_t *)(rev + (PLY_ID_LO + i) * 4) = pRevB[i];
        memcpy(spr + (PLY_ID_LO + i) * ANM_SPRITE_STRIDE, pSprB[i], ANM_SPRITE_STRIDE);
    }
    /* ... then free our slot and restore once more (the free zeroes the slot's ids
     * and resets the global sprite caches — the same proven order FreeP2CharAnm uses). */
    if (rc == 0) {
        ADDR_ANM_FREE_FN((void *)mgr, slot);
        for (i = 0; i < FACE_MAX_IDS; i++) {
            *(uint32_t *)(scr + (FACE_ID_LO + i) * 4) = fScript[i];
            *(uint32_t *)(rev + (FACE_ID_LO + i) * 4) = fRev[i];
            memcpy(spr + (FACE_ID_LO + i) * ANM_SPRITE_STRIDE, fSprS[i], ANM_SPRITE_STRIDE);
        }
        for (i = 0; i < PLY_MAX_IDS; i++) {
            *(uint32_t *)(scr + (PLY_ID_LO + i) * 4) = pScriptB[i];
            *(uint32_t *)(rev + (PLY_ID_LO + i) * 4) = pRevB[i];
            memcpy(spr + (PLY_ID_LO + i) * ANM_SPRITE_STRIDE, pSprB[i], ANM_SPRITE_STRIDE);
        }
        Log("[face-diag] slot %d freed; face + player windows restored", slot);
    }
}

/* Mark every active P2 shot inactive. Used when the different-char overlay retires:
 * P2's in-flight shots hold a direct reference (shot+0x1e4) to the now-freed char
 * anm; ZUN's shot-update loop (FUN_0043d2f0) derefs it (+0x2c/+0x30) and crashes on
 * the freed pointer. Clearing the slots removes those dangling shots; new P2 shots
 * after retirement bind P1's (live) anm and are safe. */
static void ClearP2Shots(void *p2)
{
    char *arr = (char *)p2 + OFF_SHOTS;
    int i, cleared = 0;
    for (i = 0; i < SHOT_COUNT; i++) {
        short *act = (short *)(arr + i * SHOT_STRIDE + OFF_SHOT_ACTIVE);
        if (*act) { *act = 0; cleared++; }
    }
    if (cleared) Log("retire: cleared %d P2 shots (would dangle the freed char anm)", cleared);
}

/* Exchange the 0x400-range table entries between P1's (live) and P2's captured
 * char for exactly the ids P2 defines. enter=1 installs P2, enter=0 restores
 * P1. Re-entrancy-guarded; only acts while P2 has a different-char anm.
 *
 * SAFETY: our captured script pointers (s_p2Script) point INTO the anm file
 * loaded in s_p2AnmSlot. If the engine ever reused that slot (it shouldn't now
 * that we avoid kGameAnmSlots, but some path might), those pointers dangle and
 * swapping them in would crash on the next draw. So before installing P2, verify
 * the slot still holds OUR file; if not, retire the different-char overlay (P2
 * falls back to rendering P1's char — safe) and log it. CRITICAL: also clear P2's
 * in-flight shots, which still point at the freed anm — leaving them crashes ZUN's
 * shot-update loop on the next tick (the observed diff-char-P2 "crash on shooting"). */
static void SwapAnm(int enter)
{
    uint32_t mgr = *ADDR_ANM_MGR_PP;
    char *scr, *spr, *rev;
    int i;
    if (!s_p2AnmActive || !mgr || s_p2IdCount == 0) return;
    if (enter == s_anmSwapped) return;
    if (enter && s_p2AnmSlot >= 0) {
        uint32_t cur = *(uint32_t *)(mgr + ANM_SLOT_TBL + s_p2AnmSlot * 0xc);
        if (cur != s_p2AnmFile) {
            Log("SwapAnm: slot %d reused by engine (file 0x%08x != 0x%08x) "
                "-> retiring P2 different-char overlay (mirrors P1)",
                s_p2AnmSlot, cur, s_p2AnmFile);
            if (s_p2) ClearP2Shots((void *)s_p2);   /* kill shots that ref the freed anm */
            s_p2AnmActive = 0;
            s_anmSwapped = 0;
            return;
        }
    }
    scr = (char *)(mgr + ANM_SCRIPT_TBL);
    spr = (char *)(mgr + ANM_SPRITE_TBL);
    rev = (char *)(mgr + ANM_REV_TBL);
    for (i = 0; i < s_p2IdCount; i++) {
        int id = s_p2Ids[i];
        uint32_t *sScr = &s_p2Script[i];
        uint32_t *sRev = &s_p2Rev[i];
        uint32_t *tScr = (uint32_t *)(scr + id * 4);
        uint32_t *tRev = (uint32_t *)(rev + id * 4);
        unsigned char *tSpr = (unsigned char *)(spr + id * ANM_SPRITE_STRIDE);
        uint32_t tmp; unsigned char tmpS[ANM_SPRITE_STRIDE];
        tmp = *tScr; *tScr = *sScr; *sScr = tmp;            /* swap script ptr  */
        tmp = *tRev; *tRev = *sRev; *sRev = tmp;            /* swap reverse-base */
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
            /* §8b probe (opt-in, once per char): log the correct-face load's
             * 0x400-corruption fingerprint, then self-restore. No gameplay effect. */
            if (s_p2FaceDiag && s_p2FaceDiagDone != sel / 2) {
                LoadP2FaceDiag(sel / 2);
                s_p2FaceDiagDone = sel / 2;
            }
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
        *(uint32_t *)(pp + OFF_DEATH_TIMER) = *(uint32_t *)(p1 + OFF_DEATH_TIMER);
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
        *(uint32_t *)(pp + OFF_DEATH_TIMER) = *(uint32_t *)(sht + 8);
    }
    Log("P2 loadout: %s (P1 = %s)", ADDR_SEL_NAMES[sel], ADDR_SEL_NAMES[p1sel]);
}

static void SpawnP2(void)
{
    if (s_suppressP2) { Log("spawn: SUPPRESSED (coop.ini suppress_p2=1, desync test)"); return; }
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
            int p2char = ((s_p2Sel >= 0 ? s_p2Sel : *ADDR_SEL_ID) / 2) % 3;
            s_p2Lives = *(float *)(res + RES_LIVES);
            s_p2Power = *(float *)(res + RES_POWER);
            /* bombs: P2's own CHARACTER default, not a copy of P1's count */
            s_p2Bombs = kCharStartBombs[p2char];
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
    s_enemyCount = 0;                       /* drop the enemy snapshot (stale ptrs) */
    s_declSuppressFace = 0;                 /* no P2 declaration to hide once gone   */
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

/* ---- proximity transparency (NIGHT_SHIFT #2) ----
 * When the two players overlap you can lose track of your own character. Fade
 * the OTHER player out as they get close, so the local one stays clear.
 * ASYMMETRIC / per-instance: on the host (local = P1) P2 fades; on the guest
 * (local = P2) P1 fades. Single-machine (no netplay) keys off P1 as "local",
 * i.e. P2 fades near P1 — a prototype of the real netplay-asymmetric effect.
 * The fade ramps with the P1<->P2 distance (opaque far, ~transparent near) but
 * never goes fully invisible (PROX_FLOOR), so the faded player is still
 * trackable. Distance is compared squared (no sqrt / math.h dep). Applied AFTER
 * the player update (which rewrites the tint), gated off ghosts/game-over.
 * Off by default; flip coop.ini [coop] proximity_fade=1. Most meaningful under
 * netplay — tune PROX_* + PROX_FLOOR after a look. */
/* Range kept tight on purpose: the player sprite is ~32px wide, so fading should
 * only kick in once the two are actually overlapping ("getting on top" of each
 * other), not from a screen apart. Fade begins at 48px center-to-center (sprites
 * just touching) and reaches the floor by 16px (heavily stacked). */
#define PROX_NEAR2  (16.0f * 16.0f)    /* <= this: floor alpha (most transparent) */
#define PROX_FAR2   (48.0f * 48.0f)    /* >= this: fully opaque                   */
#define PROX_FLOOR  0x40u              /* minimum alpha when fully overlapped     */

/* Deferred focus-ring fade (see ApplyProximityFade / HookedDraw). The sprite tint
 * written in the update phase survives to the draw, but the focus-RING effect's
 * colour (effect+0x1b8) is rewritten by the effect-manager's own update task —
 * whose order vs the player update is undefined — so an update-phase write to it
 * gets clobbered before the draw (this was the "only the sprite fades" bug). We
 * stash the tint in the update and re-apply it at DRAW time (after ALL updates,
 * just before the effect draws). */
static uint32_t s_proxFxTint       = 0xffffffffu;
static int      s_proxFxRemoteIsP2 = 1;   /* 1 = fade P2's ring (host/single), 0 = P1's (guest) */
static int      s_proxFxPending    = 0;   /* set each frame the fade is active; consumed in draw */
static int      s_proxFxLogged     = 0;   /* one-shot diagnostic of the draw-time apply           */

/* Fade a player's FOCUS-mode indicator (the rotating ring + central hitbox dot,
 * effect type 0x18) to match the sprite fade. The handle lives at player+0x9d8;
 * its ARGB tint is at fx+0x1b8 (the focus updater FUN_0041abe0 only moves it, but
 * the effect's anm task may rewrite the colour — hence the draw-time apply). The
 * focused remote player's hitbox is at least as distracting as the sprite when
 * the two overlap, so it gets the same alpha. No-op when that player isn't
 * focusing (no live focus effect). */
static void FadePlayerFocusFx(void *player, uint32_t tint)
{
    uint32_t fx = *(uint32_t *)((char *)player + OFF_PLAYER_FX);
    if (fx && *(unsigned char *)(fx + OFF_FX_TYPE) == FX_TYPE_FOCUS)
        *(uint32_t *)(fx + OFF_FX_COLOR) = tint;
}

static void ApplyProximityFade(void *p2)
{
    float *p1pos = (float *)((char *)ADDR_PLAYER_BASE + OFF_POS_X);
    float *p2pos = (float *)((char *)p2 + OFF_POS_X);
    float dx = p1pos[0] - p2pos[0], dy = p1pos[1] - p2pos[1];
    float d2 = dx * dx + dy * dy;
    unsigned int a;
    if      (d2 >= PROX_FAR2)  a = 0xff;
    else if (d2 <= PROX_NEAR2) a = PROX_FLOOR;
    else {
        float t = (d2 - PROX_NEAR2) / (PROX_FAR2 - PROX_NEAR2);   /* 0..1 */
        a = PROX_FLOOR + (unsigned int)(t * (float)(0xff - PROX_FLOOR));
    }
    uint32_t tint = (a << 24) | 0x00ffffffu;
    /* Sprite tint is written here (update phase, after the player's own anm, so it
     * sticks). The focus-RING tint is DEFERRED to HookedDraw — writing it here gets
     * clobbered by the effect's anm task before the draw (see s_proxFx* notes). */
    if (s_netActive && !Nc_IsHost()) {
        /* guest: local = P2, so fade the REMOTE P1 (sprite + focus hitbox); keep P2 opaque */
        *(uint32_t *)((char *)ADDR_PLAYER_BASE + OFF_TINT) = tint;
        *(uint32_t *)((char *)p2 + OFF_TINT) = 0xffffffffu;
        s_proxFxRemoteIsP2 = 0;
    } else {
        /* host / single-machine: local = P1, so fade the REMOTE P2 (sprite + focus hitbox) */
        *(uint32_t *)((char *)p2 + OFF_TINT) = tint;
        s_proxFxRemoteIsP2 = 1;
    }
    s_proxFxTint    = tint;
    s_proxFxPending = 1;
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
#define REVIVE_RADIUS  32.0f
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
            /* D1 (user-confirmed): NO free revive — the reviver must have a spare
             * extend to spend. Without one, the confirm does nothing and the
             * channel stays charged so the revive fires the instant they earn a
             * life. (The last-life-death 1up drop is unaffected — kept.) */
            if (reviverIsP1) {
                uint32_t res = *ADDR_RES_PTR;
                if (res && *(float *)(res + RES_LIVES) >= 1.0f) {
                    *(float *)(res + RES_LIVES) -= 1.0f;    /* donate one */
                    ADDR_HUD_REFRESH(ADDR_SCORE_SINGLETON); /* re-heal checksum */
                    Log("revive: P1 donated a life (%.0f spare left)",
                        *(float *)(res + RES_LIVES));
                    ReviveP2();
                    s_reviveFrames = 0;
                } else {
                    Log("revive: P1 has no spare life -> cannot revive (D1)");
                }
            } else {
                if (s_p2Lives >= 1.0f) {
                    s_p2Lives -= 1.0f;
                    Log("revive: P2 donated a life (%.0f spare left)", s_p2Lives);
                    ReviveP1();
                    s_reviveFrames = 0;
                } else {
                    Log("revive: P2 has no spare life -> cannot revive (D1)");
                }
            }
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
    /* item 5: the F2-F12 dev hotkeys are behind coop.ini [coop] debug_keys (default
     * OFF). Many mutate co-op state (spawn/despawn/char-swap/killable) and would
     * desync a net game — keep them off for ordinary players. */
    if (!s_debugKeys) return;
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
    int f12 = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
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
    if (f12 && !s_prevF12) { s_p2IconHud = !s_p2IconHud; Log("P2 HUD style -> %s", s_p2IconHud ? "ICONS" : "TEXT"); }
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
    s_prevF12 = f12;
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
    CRUMB2("collideBullet", 0);
    int r = (isP1 && s_p1Ghost) ? 0                       /* ghost P1: untouchable */
          : s_origCollideBullet(self, edx, pos, size);    /* P1 (unchanged) */
    void *p2 = (void *)s_p2;
    if (s_p2Killable && p2 && isP1 && !s_p2Ghost && !P2CollisionSkipped(p2)) {
        CRUMB2("collideBullet:P2", 1);
        s_origCollideBullet(p2, edx, pos, size);          /* P2 — param-relative */
    }
    return r;
}

static int __fastcall HookedCollideLaser(void *self, void *edx,
                                         void *a, void *b, void *c, void *d, int e)
{
    int isP1 = ((uint32_t)self == ADDR_PLAYER_BASE);
    CRUMB2("collideLaser", 0);
    int r = (isP1 && s_p1Ghost) ? 0
          : s_origCollideLaser(self, edx, a, b, c, d, e); /* P1 (unchanged) */
    void *p2 = (void *)s_p2;
    if (s_p2Killable && p2 && isP1 && !s_p2Ghost && !P2CollisionSkipped(p2)) {
        CRUMB2("collideLaser:P2", 1);
        s_origCollideLaser(p2, edx, a, b, c, d, e);        /* P2 */
    }
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
    CRUMB2("graze", 0);
    int r = (isP1 && s_p1Ghost) ? 0                       /* ghost P1: no grazing */
          : s_origGraze(self, edx, pos, size);            /* P1 (unchanged) */
    void *p2 = (void *)s_p2;
    if (p2 && isP1 && !s_p2Ghost && !P2CollisionSkipped(p2)) {
        CRUMB2("graze:P2", 1);
        int r2 = s_origGraze(p2, edx, pos, size);          /* P2 — param-relative */
        /* FUN_0043e3b0 returns 2 = "bullet sits in one of this player's clear regions"
         * (P2+0x17dc) — the SAME field check FUN_0043e0a0 the hit test uses. The bullet
         * loop runs THIS graze test (not the hit test) for every un-grazed older bullet
         * (PCBdecomp.c:14718), so it is the path that cancels most of a bomb's bullets.
         * B4 BUG (fixed): we only forwarded r2==1 (graze) and DROPPED r2==2, so P2's
         * bomb/border field never cancelled bullets that weren't also near P2's graze box
         * — exactly "P2 balls don't clear bullets, except the rare one P2 also grazed".
         * Forward the cancel: 2 (cleared) takes priority over 1 (grazed) over 0. Reads
         * only synced sim state (P2 regions + position), so it stays lockstep-safe. */
        if (r2 == 2) r = 2;             /* bullet in P2's clear field -> cancel it */
        else if (r == 0 && r2 == 1) r = 1;   /* P2 grazed -> mark bullet grazed so its hit test runs */
    }
    return r;
}

/* B4 — route P2's bomb clear-region registrations onto P2's own +0x17dc array.
 * While inside P2's update window, force ECX to P2 so a region (circle/box) the bomb
 * callback would otherwise stamp on the P1 static base lands on P2 instead. No-op when
 * the call was already P2's; never touches P1's own bomb (s_inP2Update == 0 then). */
static void *__fastcall HookedAddClearCircle(void *self, void *edx, void *pos,
                                             uint32_t r, uint32_t col, uint32_t type)
{
    if (s_p2BombClear && s_inP2Update && s_p2 && !s_p2Ghost && self != (void *)s_p2)
        self = (void *)s_p2;
    return s_origAddClearCircle(self, edx, pos, r, col, type);
}

static void *__fastcall HookedAddClearBox(void *self, void *edx, void *pos,
                                          uint32_t w, uint32_t h, uint32_t type)
{
    if (s_p2BombClear && s_inP2Update && s_p2 && !s_p2Ghost && self != (void *)s_p2)
        self = (void *)s_p2;
    return s_origAddClearBox(self, edx, pos, w, h, type);
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

/* B1 — point-item EXTEND mirrored to the partner.
 * A point-item threshold extend (PCBdecomp.c case 1, FUN_0042d83a inside the
 * `+0x2c < +0x30` loop) bumps the extend-TIER counter res+0x2c and grants a
 * life-or-bomb to whoever collected the triggering point item (the engine writes
 * the resource struct, which the swap routes to the collector). B1: that extend
 * should go to BOTH players. We detect it by the tier delta — the 1up ITEM
 * (case 5) calls FUN_0042d83a WITHOUT touching res+0x2c, so it stays
 * collector-specific (correct). The extend fn reads x87 ST0 set by its caller, so
 * it CANNOT be C-hooked (a detour clobbers ST0 before the trampoline reads it);
 * the tier-delta route is FPU-safe and reuses the existing swap.
 *
 * Must run while the triggering item's swap is still in place (BEFORE the next
 * RestoreHeldRes) so s_p2ResHeld names the collector. The partner gets the extend
 * appropriate to ITS OWN state (life if <8, else a bomb if <8 — ZUN's overflow). */
#define COOP_MAX_LIVES 8.0f
#define COOP_MAX_BOMBS 8.0f
#define RES_EXTEND_TIER 0x2c
static int s_lastExtendTier = -1;

static void CheckPartnerExtend(void)
{
    uint32_t res = *ADDR_RES_PTR;
    if (!res) return;
    int tier = *(int *)(res + RES_EXTEND_TIER);
    if (s_lastExtendTier < 0) { s_lastExtendTier = tier; return; }
    if (tier > s_lastExtendTier && s_p2 && s_p2SepRes) {
        int n = tier - s_lastExtendTier;
        for (; n > 0; n--) {
            if (s_p2ResHeld) {
                /* P2 collected -> already credited to P2; mirror to P1 (its saved
                 * values, written back + checksum-healed by RestoreHeldRes). */
                if (s_heldP1Lives < COOP_MAX_LIVES)      s_heldP1Lives += 1.0f;
                else if (s_heldP1Bombs < COOP_MAX_BOMBS) s_heldP1Bombs += 1.0f;
            } else {
                /* P1 collected -> already credited to P1; mirror to P2. */
                if (s_p2Lives < COOP_MAX_LIVES)      s_p2Lives += 1.0f;
                else if (s_p2Bombs < COOP_MAX_BOMBS) s_p2Bombs += 1.0f;
            }
        }
        Log("B1: point-item extend mirrored to partner (%s collected, tier=%d)",
            s_p2ResHeld ? "P2" : "P1", tier);
    }
    s_lastExtendTier = tier;
}

static int __fastcall HookedCollectOverlap(void *self, void *edx, float *pos, float *size)
{
    CheckPartnerExtend();                              /* B1: mirror the prev item's extend */
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

/* B3 — P2 autocollect above the point-of-collection line.
 * ZUN's per-item autocollect trigger (FUN_00432990, PCBdecomp.c:20391) sets an item
 * to homing-mode when `(P1 full power || Extra/Phantasm) && (P1_Y < line)`, keyed to
 * P1 ONLY — so P2 never vacuums items by going up top, even on Extra. We replicate
 * the SAME gate for P2: when P2 is above the line and (P2 full power || difficulty
 * > Lunatic), set the homing-mode byte on each active item for which P2 is the nearer
 * player. The engine's next-frame homing then carries it in, and HookedAngleToPlayer
 * already redirects the homing target to the nearer player (so it lands on P2). We
 * only ever WRITE the mode byte (no FPU, no RNG) -> determinism- and tamper-safe.
 * (The full-power half is per-player only with s_p2SepRes on; otherwise P2 shares
 * P1's power, in which case the engine's own P1 trigger already covers it.) */
#define ADDR_GAME_CFG_PP ((volatile uint32_t *)0x00575948) /* -> config; +0x20 = collect line Y */
/* ADDR_DIFFICULTY defined near the top with the other pinned globals */
#define ITEM_STRIDE      0x288
#define ITEM_OFF_POSX    0x24c
#define ITEM_OFF_POSY    0x250
#define ITEM_OFF_ACTIVE  0x27d
#define ITEM_OFF_MODE    0x27f      /* 0 pop-fall, 1 homing, 2 scatter */
#define ITEM_COUNT       1100
#define COOP_FULL_POWER  128.0f
static int s_p2Autocollect = 1;     /* B3: P2 line-cross / Extra autocollect (default on) */

static void ApplyP2Autocollect(void *mgr)
{
    void *p2 = (void *)s_p2;
    if (!s_p2Autocollect || !p2 || s_p2Ghost || !mgr) return;

    uint32_t cfg = *ADDR_GAME_CFG_PP;
    if (!cfg) return;
    float line = *(float *)(cfg + 0x20);
    int   diff = (int)*ADDR_DIFFICULTY;

    char *pp = (char *)p2, *p1 = (char *)ADDR_PLAYER_BASE;
    float p2y  = *(float *)(pp + OFF_POS_Y);
    float p2pw = s_p2Power;
    if (!s_p2SepRes) {                            /* shared power: P1's trigger already covers it */
        uint32_t res = *ADDR_RES_PTR;
        p2pw = res ? *(float *)(res + RES_POWER) : 0.f;
    }
    /* same shape as ZUN's gate, for P2 */
    if (!((p2pw >= COOP_FULL_POWER || diff > 3) && p2y < line)) return;

    float ax = *(float *)(p1 + OFF_POS_X), ay = *(float *)(p1 + OFF_POS_Y);
    float bx = *(float *)(pp + OFF_POS_X), by = p2y;
    int i;
    for (i = 0; i < ITEM_COUNT; i++) {
        char *it = (char *)mgr + i * ITEM_STRIDE;
        if (*(unsigned char *)(it + ITEM_OFF_ACTIVE) == 0) continue;
        unsigned char mode = *(unsigned char *)(it + ITEM_OFF_MODE);
        if (mode == 1 || mode == 2) continue;     /* already homing / scattering */
        float ix = *(float *)(it + ITEM_OFF_POSX), iy = *(float *)(it + ITEM_OFF_POSY);
        float d1 = (ax - ix) * (ax - ix) + (ay - iy) * (ay - iy);
        float d2 = (bx - ix) * (bx - ix) + (by - iy) * (by - iy);
        if (d2 < d1)                              /* P2 is the nearer player -> vacuum to P2 */
            *(unsigned char *)(it + ITEM_OFF_MODE) = 1;
    }
}

/* Bracket the item-update loop so the LAST collected item's P2 swap is undone
 * before anything outside the loop reads the shared resources. */
static void __fastcall HookedItemLoop(void *self)
{
    s_itemMgr = self;                   /* captured for DropOneUp */
    s_origItemLoop(self);
    CheckPartnerExtend();               /* B1: catch the last item's extend (swap still in) */
    RestoreHeldRes();
    ApplyP2Autocollect(self);           /* B3: P2 line-cross / Extra autocollect */
}

/* ---- suppress P2's bomb-declaration portrait (different-char P2) ----
 * The bomb spell declaration shows the GLOBAL char's face from data/face_*00.anm
 * (loaded once for P1's char at stage start). For a DIFFERENT-char P2 that face
 * is wrong, and loading P2's face is the unsolved §8b problem. So instead we HIDE
 * just the face portrait for a P2 declaration, keeping the spell name + bar + bomb
 * sound — informative and crash-free (no anm load/swap, just a draw-gate bit).
 *   create  FUN_0042868d(this, faceSprite, name) runs inside the bomb cb, i.e.
 *           inside P2's update window -> s_inP2Update marks ownership.
 *   draw    FUN_0042c577(this) draws the player portrait block only when
 *           declBase+0x590c bit0 is set (PCBdecomp 17300-17304). We clear that bit
 *           around the original when the live declaration is a different-char P2's,
 *           then restore it; the spell-name block (gated separately at +0x66d4,
 *           17323) is untouched and still draws. P1's declaration is never changed. */
typedef void (__fastcall *DeclMakeFn_t)(void *self, void *edx, int faceSprite, char *name);
typedef void (__fastcall *DeclDrawFn_t)(void *self);
#define ADDR_DECL_MAKE    ((LPVOID)0x0042868d) /* __thiscall(this,faceSprite,name) */
#define ADDR_DECL_DRAW    ((LPVOID)0x0042c577) /* __fastcall(ecx = decl manager)   */
#define DECL_OBJ_OFF      8        /* this+8 -> the UI object base                 */
#define DECL_PORTRAIT_FLG 0x590c   /* declBase+: bit0 gates the face-portrait draw */
static DeclMakeFn_t s_origDeclMake = NULL;
static DeclDrawFn_t s_origDeclDraw = NULL;
/* s_hideP2Portrait / s_declSuppressFace / s_declSuppressLogged declared earlier
 * (near s_perPlayerAim) so DespawnP2 can clear the flag. */

static void __fastcall HookedDeclMake(void *self, void *edx, int faceSprite, char *name)
{
    s_origDeclMake(self, edx, faceSprite, name);
    /* whoever's update window we're in owns this declaration; only a different-char
     * P2 (s_p2AnmActive) shows the wrong face and needs hiding. */
    s_declSuppressFace = (s_hideP2Portrait && s_inP2Update && s_p2AnmActive) ? 1 : 0;
    if (s_declSuppressFace && !s_declSuppressLogged) {
        Log("decl portrait: hiding P2's bomb face (P2 is a different char)");
        s_declSuppressLogged = 1;
    }
}

static void __fastcall HookedDeclDraw(void *self)
{
    if (s_declSuppressFace && self) {
        uint32_t declBase = *(uint32_t *)((char *)self + DECL_OBJ_OFF);
        if (declBase) {
            volatile uint32_t *flg = (volatile uint32_t *)(declBase + DECL_PORTRAIT_FLG);
            uint32_t saved = *flg;
            *flg = saved & ~1u;          /* hide the face block for this draw only */
            s_origDeclDraw(self);
            *flg = saved;                /* restore — never permanently corrupt    */
            return;
        }
    }
    s_origDeclDraw(self);
}

/* B5 boss-spell gate — CORRECTED SIGNAL (2026-06-18, via the clean-room EoSD decomp).
 * The first attempts gated on the score-manager's spell-DECLARATION index
 * (*(0x0049fbf8)+0x1fbac, ZUN's FUN_0042ad66); in-game that read -1 (no spell) while a P2
 * bomb was killing a boss spell, so it was the WRONG field. EoSD names the real one:
 * `g_EnemyManager.spellcardInfo.isActive` (set =1 by the ECL spellcard opcode at spell
 * start, EclManager.cpp:731; =0 at end) drives the boss damage model in EnemyManager.cpp:
 *   if (isActive) { if (out==0) dmg/=7; else if (usedBomb) dmg/=3; else dmg=0; }
 * and the bomb handler does `usedBomb = isActive` (Player.cpp:364). PCB is the same engine:
 * the damage code at PCBdecomp.c:12867-12899 reads `DAT_009545c8 + param_1` (isActive) and
 * `DAT_009545dc + param_1` (usedBomb). param_1 is the enemy-manager context base. We first
 * assumed it was 0 (absolute globals), but an in-game @hit dump read scActive=0 with a real
 * boss present — param_1 is genuinely NON-ZERO, so 0x9545c8/0x9545dc are DISPLACEMENTS off
 * that base, not absolute addresses. We capture the live base at runtime (HookedEnemyUpdate,
 * = ECX of FUN_00420620, verified stored at [ebp-0x1e8] for the read at 0x421387) and read
 * isActive/usedBomb relative to it (SC_OFF_* below). (The transform that "removes the boss
 * hitbox" is the dmg=0 leaf of that same model.) */
#define SC_OFF_ISACTIVE   0x9545c8   /* enemy-manager spellcardInfo.isActive (from base) */
#define SC_OFF_USEDBOMB   0x9545dc   /* enemy-manager spellcardInfo.usedBomb             */
#define ADDR_SPELL_MGR_PTR ((volatile uint32_t *)0x0049fbf8) /* (old decl-index path, diag only) */
#define SPELL_OFF_INDEX    0x1fbac
static int SpellcardActive(void)
{
    uint32_t b = s_enemyMgr;
    return b ? (*(volatile int *)(b + SC_OFF_ISACTIVE) != 0) : 0;
}
static int SpellcardUsedBomb(void)
{
    uint32_t b = s_enemyMgr;
    return b ? *(volatile int *)(b + SC_OFF_USEDBOMB) : 0;
}
static int BossSpellIndexRaw(void)   /* old signal, kept for the diagnostic only */
{
    uint32_t mgr = *ADDR_SPELL_MGR_PTR;
    return mgr ? *(volatile int *)(mgr + SPELL_OFF_INDEX) : 0x7fffffff;
}

/* Capture the enemy-manager base (ECX = param_1) so SpellcardActive/UsedBomb can read its
 * spellcardInfo fields. Just records the pointer and forwards — no behaviour change. */
static int __fastcall HookedEnemyUpdate(void *self)
{
    s_enemyMgr = (uint32_t)self;
    int r = s_origEnemyUpdate(self);
    /* Refuel window: hold it OPEN for the whole boss spell and for CAPTURE_WIN_FRAMES
     * after it ends. The point-substituted reward (type 1) can land at the capture instant
     * (sc still 1 mid-update) OR in the sc=0 gap after the bonus animation; spanning both
     * makes the type-1 -> power conversion fire regardless of which frame it spawns on.
     * Only the type-1 boss reward is converted (cancel stars are type 6, untouched), and
     * bosses don't drop type-1 mid-spell, so holding it open across the spell is safe. */
    if (SpellcardActive())
        s_captureWin = CAPTURE_WIN_FRAMES;
    else if (s_captureWin > 0)
        s_captureWin--;
    return r;
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
    CRUMB2("damage", 0);
    int r = s_origDamage(self, edx, pos, size, out_flag);
    void *p2 = (void *)s_p2;

    /* ENEMY SNAPSHOT for P2's per-player aim/homing (BuildP2TargetBlock): the
     * engine calls this once per damageable enemy with pos = enemy+0x2b0c, so
     * record the enemy base. Only on the engine's P1 call (self == P1); the P2
     * re-invoke below passes self = P2 and must not double-record. The two box
     * calls per enemy (primary + secondary) share the same pos -> skip the
     * consecutive duplicate. Reset is done once consumed, in HookedDraw. */
    if (s_perPlayerAim && p2 && !s_p2Ghost && (uint32_t)self == ADDR_PLAYER_BASE && pos) {
        void *en = (void *)((char *)pos - ENEMY_OFF_POS);
        if (s_enemyCount < COOP_MAX_ENEMY_SNAP &&
            (s_enemyCount == 0 || s_enemySnap[s_enemyCount - 1] != en))
            s_enemySnap[s_enemyCount++] = en;
    }

    /* P2 SHOT + BOMB DAMAGE: the per-enemy sweep is hardwired to ECX = the
     * static P1 (bisect-proven), so re-invoke param-relative for a live P2 —
     * its own array's shots, its lasers, and its bomb all get tested. With the
     * shot transfer OFF (the default now), each shot lives in exactly one
     * array, so nothing is double-counted.
     *
     * SwapAnm MUST wrap this re-invoke too (not just SwapSelGlobals): the sweep
     * REBINDS a shot's anm on hit — FUN_0043d9e0 @PCBdecomp.c:25902 does
     * FUN_0044ea20(shot, *(mgr + 0x28ef0 + (shot[+0x1d8]+0x20)*4)), reading the
     * 0x400-range script table that SwapAnm owns. Without the swap, a different-
     * char P2 (e.g. P1 Sakuya / P2 Reimu) indexes P1's table with P2's hit-id and
     * binds a wrong/garbage script ptr -> intermittent crash a few frames later
     * when that script runs (the C1 "Reimu bullets/bomb hit an enemy" crash). The
     * player update + draw already wrap their P2 calls in SwapAnm; this is the one
     * P2 engine call that was missing it. No-op for a same-char P2 (s_p2AnmActive
     * == 0 -> SwapAnm returns immediately), so default/same-char play is unchanged. */
    if (p2 && (uint32_t)self == ADDR_PLAYER_BASE && !s_p2Ghost) {
        int f2 = 0;
        int r2;
        CRUMB2("damage:P2-reinvoke", 1);   /* the C1/C2 anm-rebind hotspot */
        SwapSelGlobals(1);
        SwapAnm(1);
        r2 = s_origDamage(p2, edx, pos, size, &f2);
        SwapAnm(0);
        SwapSelGlobals(0);
        CRUMB2("damage", 0);
        if (r2 > 0) r += r2;
        if (f2 && out_flag && !*out_flag) *out_flag = f2;
    }

    /* N2 — 2P team DPS damper, with a launcher-selectable mode:
     *   FLAT (default): every enemy takes COOP_DAMPER_FLAT (0.75) — a light, uniform
     *     reduction so 2x DPS doesn't melt everything.
     *   BOSS-ONLY (damper_boss_only=1): only lifebar enemies (bosses/midbosses,
     *     enemy+0x2e29 bit6) get COOP_DAMPER_BOSS (0.60); stage popcorn takes FULL
     *     damage — fixes the "stage enemies feel tanky / can't solo the Extra opening
     *     ball line" complaint while still pacing boss fights. Boss-ness is read from
     *     the same enemy snapshot offset HookedDamage already uses (pos - ENEMY_OFF_POS). */
    if (s_bossHpScale && s_p2 && r > 0) {
        float f = COOP_DAMPER_FLAT;
        if (s_damperBossOnly) {
            int isBoss = 0;
            if (pos) {
                unsigned char fl = *(unsigned char *)((char *)pos - ENEMY_OFF_POS + ENEMY_OFF_FLAGS);
                isBoss = (fl & ENEMY_FLAG_BOSS) != 0;
            }
            f = isBoss ? COOP_DAMPER_BOSS : 1.0f;
        }
        if (f != 1.0f) {
            r = (int)(r * f);
            if (r == 0) r = 1;
        }
    }

    /* B5: Extra/Phantasm boss invincibility during a SPELL CARD, mirrored for a P2 bomb.
     * In vanilla you can't cheese an Extra/Phantasm boss spell with a bomb — the boss
     * transforms into an invincible ball (the dmg=0 leaf of ZUN's spellcard damage model,
     * EoSD EnemyManager.cpp / PCBdecomp.c:12867-12899). That model keys off the ENEMY-
     * MANAGER spellcard flag `spellcardInfo.isActive` (DAT_009545c8) — NOT the score-mgr
     * declaration index we read before, which is -1 here. So: while P2 is bombing on
     * Extra/Phantasm and the damaged enemy is a boss whose spellcard is active, zero the
     * sweep's return (the sweep already ran, so shots are still consumed + sparked, exactly
     * like ZUN's own apply-gate). Not on nonspell / no-boss / stage popcorn. */
    if (r > 0 && (uint32_t)self == ADDR_PLAYER_BASE && p2 && !s_p2Ghost) {
        int bombing = *(volatile int *)((char *)p2 + OFF_BOMBING) != 0;  /* cheapest, most selective */
        int diff    = bombing ? (int)*ADDR_DIFFICULTY : 0;
        if (bombing && (diff == 4 || diff == 5)) {
            unsigned char fl = pos ? *(unsigned char *)((char *)pos - ENEMY_OFF_POS + ENEMY_OFF_FLAGS) : 0;
            int isBoss = (fl & ENEMY_FLAG_BOSS) != 0;
            /* One-shot per bomb: dump the decision-point state so ONE Extra/Phantasm run
             * confirms the corrected signal — scActive/usedBomb (read off the live enemy-mgr
             * base, mgr=0x%08x) should be nonzero on a spell, the old declIdx stays -1.
             * enBase is the boss ENEMY base; mgr is the captured enemy-MANAGER base. */
            if (isBoss && !s_b5HitLogged) {
                s_b5HitLogged = 1;
                Log("B5 diag@hit: scActive=%d usedBomb=%d declIdx=%d flags=0x%02x "
                    "(b3invuln=%d b4dmg=%d b6boss=1) r=%d enBase=0x%08x mgr=0x%08x",
                    SpellcardActive(), SpellcardUsedBomb(), BossSpellIndexRaw(), fl,
                    (fl >> 3) & 1, (fl >> 4) & 1, r,
                    pos ? (uint32_t)((char *)pos - ENEMY_OFF_POS) : 0,
                    (uint32_t)s_enemyMgr);
            }
            if (isBoss && SpellcardActive()) {
                r = 0;
                s_b5ZeroCount++;          /* B5 diag: count fires per bomb (see HookedUpdate) */
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
/* STAGE START: the logic-frame counter went BACKWARDS = a fresh stage/game
 * task. The P2 clone's internal pointers reference the PREVIOUS stage's
 * assets, which the load just recycled (proven crash: stale P2 locked in
 * place, then crashed on shoot in stage 2) — rebuild the co-op session:
 * despawn the stale clone, re-arm auto-spawn (P2 returns ~3s in, cloned from
 * the FRESH P1). Resources carry across stages; after a game over the next
 * run reseeds from P1. A ghost is revived by the transition (still 0 spares). */
static int __fastcall HookedFrameTask(int *self)
{
    /* LOCAL co-op REPLAY RECORD: s_origFrameTask (FUN_00442cd0) saves g_InputMenu to the
     * .rpy buffer each frame. Under netplay HookedSceneTick already merged P2 into g_InputMenu
     * (high 7 bits) before this runs, so net replays carry P2. LOCAL co-op had NO such seam —
     * g_InputMenu held P1 only, so a local .rpy recorded P1's stream and ZERO P2 input. On
     * playback P2 then spawned but never moved or shot, and the co-op sim diverged ("resembles
     * gameplay but desyncs"). Lay P2 into the high bits HERE, before the record reads g_InputMenu,
     * and capture the exact word (this task always runs before the player update) so the live
     * update below uses the identical value — record == live == playback. Playback registers a
     * DIFFERENT task (FUN_00442ee0), so this hook never runs during playback; gating is belt-only. */
    if (!s_netActive && !IsReplayPlayback() && s_p2 && !s_p2Ghost) {
        s_p2LocalIn = ReadP2InputLocal();
        *ADDR_INPUT_MENU = (uint16_t)((*ADDR_INPUT_MENU & 0x01FF) | PackP2(s_p2LocalIn));
    } else {
        s_p2LocalIn = 0;
    }

    /* §8aj: per-frame seed capture. ZUN's record task FUN_00442cd0 advances the input-stream head
     * (self+0x84) by 4 and writes a 4-byte entry: input at +0, a 2-byte "flags" half at +2 that is
     * ALWAYS 0 (DAT_0062f640 is never set) and that playback (FUN_00442ee0) NEVER reads. We hijack
     * that free half to carry the frame-START RNG seed: this task runs before the frame's enemy/
     * bullet sim draws any rand (order: HookedGameStart seed-force -> THIS -> player update -> sim),
     * so *ADDR_RNG_SEED here is exactly the seed the sim will run from. Storing it per frame lets
     * playback restore the seed every frame (HookedPlayTask) and stay bit-identical to the live run
     * regardless of any rand-CALL-COUNT divergence (the residual §8ai desync: playback's P2 sim drew
     * a different number of rands than the live netcode-driven P2, slipping the seed phase). Net co-op
     * recordings only; the saved .rpy round-trips it (same compressed body as the §8ah stage block). */
    unsigned char *rpyHead0 = (s_netActive && !s_suppressP2 && !IsReplayPlayback())
                                  ? *(unsigned char **)((char *)self + 0x84) : NULL;
    int r = s_origFrameTask(self);
    if (rpyHead0) {
        unsigned char *rpyHead1 = *(unsigned char **)((char *)self + 0x84);
        if (rpyHead1 == rpyHead0 + 4) {            /* the task wrote a fresh per-frame entry */
            *(unsigned short *)(rpyHead1 + 2) = *ADDR_RNG_SEED;   /* frame-start seed -> free flags half */
            if (++s_rpySeedStored == 1)
                Log("replay: §8aj per-frame seed capture ON (frame-start seed -> each entry's free "
                    "flags half; net co-op recording) — playback will restore the seed every frame");
        }
    }
    int f = *self;
    uint32_t res = *ADDR_RES_PTR;
    int score = res ? *(int *)(res + RES_SCORE) : s_lastScore;
    if (f < s_lastLogicFrame && (s_p2 || s_p1Ghost || s_runOver)) {
        /* A frame-counter reset is a stage ADVANCE (carry resources), a RETRY /
         * fresh start (reset to starting resources), or a post-game-over reset.
         * Retry restarts the run with a 0 score; a stage advance carries the
         * (only-growing) score. So a score DROP marks a fresh start. */
        int fresh = s_runOver || (res && s_lastScore > 0 && score < s_lastScore);
        if (fresh) {
            Log("frame counter reset -> FRESH start (score %d < %d) -> P2 reset",
                score, s_lastScore);
            s_p2Carry = 0;              /* reseed P2 fresh (lives from P1, char bombs) */
            /* NETPLAY: an Esc+R / continue retry stays in the SAME scene, so the
             * scene-change re-force (HookedSceneTick) never fires and the restart kept
             * the old RNG seed — a forked stage could only re-align by a full title
             * round-trip (the test logs: "couldn't sync no matter how many Esc+R").
             * Re-arm the seed force here so the next HookedGameStart re-applies the
             * shared init seed on BOTH peers, exactly like a scene reset does. Both
             * peers detect this fresh-start on the same merged-input-driven frame, so
             * the re-arm lands together. */
            if (s_netActive) s_seedForced = 0;
        } else {
            Log("frame counter reset (stage advance) -> P2 rebuild (resources carried)");
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
    s_lastScore = score;
    return r;
}

static int __fastcall HookedUpdate(void *self)
{
    int isP1 = ((uint32_t)self == ADDR_PLAYER_BASE);
    int p1Fake = 0;
    uint16_t p1GhostSavedIn = 0;
    CRUMB2("update", 0);

    /* §8ab diag: snapshot P1+P2 positions, the RNG pair and the x87 control word ~1/sec, during
     * BOTH live netplay AND co-op replay playback — both positions are last-frame-settled here
     * (pre-update). The point: diff the logs to localise the fork. Under netplay the frame label is
     * the LOCKSTEP frame (identical on both machines), so host-live vs guest-live diff exposes a
     * live cross-machine desync the RNG oracle missed; a machine's live log vs its OWN replay log
     * exposes a record/playback asymmetry in our code. P1-also-off => global FP; P2-only => graft.
     * The §8aa pin proved a no-op for th7_12, so the residual is NOT control-word precision. */
    /* §8ah-diag: log the ReplayDetTrace from the FIRST recorded frame, not just after P2 spawns.
     * The seed fork is in the stage INTRO (rf 0..~240, before P2 appears) which the old s_p2 gate
     * skipped on the record side (rec started at rf=241) — so rec/rpy couldn't be diffed there.
     * Mode bit2 (0x0062f648>>2) = "replay record/playback active" = inside a recorded/played stage,
     * true from the stage's first frame in BOTH record and playback; gate on it so rec covers the
     * intro and aligns with rpy's rf=1.. for a clean per-frame diff of the early divergence. */
    int recActive = (int)(((*(volatile uint32_t *)0x0062f648) >> 2) & 1);
    /* §8ai: reproduce the live per-scene SEEDFORCE on playback. HookedReplayLoad armed this at stage
     * load; fire ONCE at the first IN-STAGE frame (recActive), after ZUN's stage-setup has drawn its
     * ~80-95 RNG calls but before this frame's gameplay draws — wiping the setup exactly as the live
     * netplay run did, so the replayed gameplay RNG starts from the recorded initSeed. Runs before the
     * ReplayDetTrace below so the trace reflects the corrected seed. Net-recorded co-op replays only. */
    if (isP1 && recActive && s_replaySeedArm && IsReplayPlayback()) {
        unsigned short before = *ADDR_RNG_SEED;
        *ADDR_RNG_SEED = s_replayInitSeed;
        s_replaySeedArm = 0;
        Log("replay PLAYBACK: §8ai first-in-stage seed re-force 0x%04x -> 0x%04x (wipes ~%u stage-setup "
            "draws, mirrors live SEEDFORCE)", before, s_replayInitSeed, (unsigned)*ADDR_RNG_CTR);
    }
    if (isP1 && (s_p2 || recActive || (IsReplayPlayback() && s_replayIsCoop))) {
        static int s_rpyDbgTick = 0;
        int play = IsReplayPlayback();
        /* §8ah: while RECORDING, backfill the FORCED init seed into ZUN's stage-block seed slot
         * (s_rpyStageBlk+0x20) in case header-init ran before the link was up (Nc_GetInitSeed()==0
         * then, so HookedReplayHdrInit couldn't write it). Once linked, stamp it so the saved .rpy
         * reproduces the per-scene SEEDFORCE via ZUN's own restore. Cheap; idempotent — only writes
         * while the slot still differs. The header cw byte is kept for the in-memory path only. */
        if (!play && s_rpyHdrBuf) {
            s_rpyHdrBuf[RPY_CW_OFF] = (unsigned char)(0x80 | ((FpuCw() >> 8) & 0x0f));
            if (s_netActive && s_rpyStageBlk) {
                unsigned short is = (unsigned short)Nc_GetInitSeed();
                if (is && *(unsigned short *)(s_rpyStageBlk + 0x20) != is)
                    *(unsigned short *)(s_rpyStageBlk + 0x20) = is;
            }
        }
        ReplayDetTrace(play);                               /* §8ac per-frame CSV (gated by replay_trace) */
        int fr = s_netActive ? s_netFrame : s_rpyDbgTick;   /* net=lockstep frame; else stage-tick */
        if ((fr % 60) == 0) {
            void *p2d = (void *)s_p2;
            /* REC-LOC vs RPLY share the stage-tick counter (P2 auto-spawns on the same frame both
             * times), so a LOCAL record-then-replay on ONE machine diffs line-for-line — the
             * cleanest determinism test (no netcode, no seed-force, no cross-platform). cw is the
             * RAW x87 word (FpuCw) now — 0x037f=64-bit, 0x007f=24-bit; the §8ad precision lever. */
            Log("coop diag %s f=%d P1=(%.3f,%.3f) P2=(%.3f,%.3f) rng=0x%04x ctr=%u cw=0x%04x pin=%d",
                s_netActive ? "LIVE-NET" : (play ? "RPLY" : "REC-LOC"), fr,
                *(float *)((char *)ADDR_PLAYER_BASE + OFF_POS_X),
                *(float *)((char *)ADDR_PLAYER_BASE + OFF_POS_Y),
                p2d ? *(float *)((char *)p2d + OFF_POS_X) : -1.0f,
                p2d ? *(float *)((char *)p2d + OFF_POS_Y) : -1.0f,
                (unsigned)*ADDR_RNG_SEED, (unsigned)*ADDR_RNG_CTR,
                (unsigned)FpuCw(), s_fpuPinnedReplay);
        }
        s_rpyDbgTick++;
    }

    /* §8ad: re-assert the recorded precision right at the SIM seam (after the diag sampled the
     * frame-start cw, so the diag still reveals whether HookedSceneTick's pin held). This covers
     * the P1 update below AND the P2 re-invoke even if something reset the cw after frame start. */
    if (isP1 && s_replayPinCw && IsReplayPlayback())
        FpuSetCw(s_replayPinCw);

    /* B2: refresh the power->cherry suppression flag once per frame (read by the
     * item-spawner detour; inert when that hook isn't installed). Suppress while
     * P2 is live and below full power — so P1-full alone keeps drops as power. */
    if (isP1)
        g_b2Suppress = (unsigned char)(s_p2 && s_p2Power < COOP_FULL_POWER);

    /* B2 diag: confirm the detour is actually installed AND firing. (It read as "didn't
     * work" repeatedly: twice the deployed coop.ini had cherry_both_full=0 from the OLD
     * launcher; then the naked ST0 trampoline was a no-op because the spawner reads power
     * from memory, not ST0 — now a real thiscall detour.) Log the first kept-as-power. */
    if (isP1 && !s_b2LoggedFire && g_b2Suppressed) {
        s_b2LoggedFire = 1;
        Log("B2: detour FIRING — kept power item(s) as power (suppressed=%u of %u spawner calls)",
            g_b2Suppressed, g_b2Calls);
    }

    /* B5 DIAGNOSTIC: P2 "bomb armour" reportedly does nothing on Extra. Log once per
     * P2 bomb (the 0->1 edge of P2's bombing flag) the two difficulty globals (the
     * boss-damage code uses 0x62f85c for its own Extra rules; B5 reads 0x626280) and,
     * at bomb end, how many times B5 zeroed boss damage. count==0 => B5's condition
     * never matched (difficulty or bombing-flag read is wrong); count>0 but boss still
     * died => the damage isn't flowing through this return. Cheap (edge-triggered). */
    if (isP1 && s_p2 && !s_p2Ghost) {
        int b = (*(volatile int *)((char *)s_p2 + OFF_BOMBING) != 0);
        if (b && !s_p2PrevBombing) {
            s_b5ZeroCount = 0;
            s_b5HitLogged = 0;           /* re-arm the @hit dump for this bomb */
            Log("B5 diag: P2 bomb START  diff[0x626280]=%u diff[0x62f85c]=%u p2bombing=%d "
                "scActive=%d usedBomb=%d declIdx=%d mgr=0x%08x",
                *(volatile uint32_t *)0x00626280, *(volatile uint32_t *)0x0062f85c, b,
                SpellcardActive(), SpellcardUsedBomb(), BossSpellIndexRaw(), (uint32_t)s_enemyMgr);
        } else if (!b && s_p2PrevBombing) {
            Log("B5 diag: P2 bomb END    times B5 zeroed boss damage this bomb = %d", s_b5ZeroCount);
        }
        s_p2PrevBombing = b;
    }

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

        /* auto-spawn P2 as soon as P1 is controllable. P2 is a CLONE of P1, so it
         * can't exist until P1's character is loaded — P1Ready() (char-data set) is
         * the hard floor, and by the time that's true P1 is already in state 0 (the
         * stage intro that plays before the character loads is unavoidable lateness;
         * spawning P2 with its OWN fly-in would be a separate entity, deferred).
         * AUTO_SPAWN_AFTER (2) is just a tiny settle, deterministic on both machines.
         * In replay PLAYBACK only for a co-op replay (tag present); P2's char from the tag. */
        if (!s_autoSpawned && !s_p2) {
            int playback = IsReplayPlayback();
            if ((!playback || s_replayIsCoop) && P1Ready() &&
                *(unsigned char *)((char *)ADDR_PLAYER_BASE + OFF_STATE) == 0) {
                if (++s_readyFrames == AUTO_SPAWN_AFTER) {
                    if (playback) { s_p2Sel = s_replayP2Sel; s_allowDiffChar = s_replayDiffChar; }
                    Log("auto-spawn: %d ready frames reached%s", AUTO_SPAWN_AFTER,
                        playback ? " (replay playback)" : "");
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
            /* §8ak: P2's input source per mode. PLAYBACK must read the recorded word the playback
             * task (FUN_00442ee0) is ABOUT to apply THIS frame — the replay buffer's current head —
             * NOT g_InputGameplay. The playback task runs AFTER this player update, so during the
             * update g_InputGameplay still holds the PREVIOUS frame's word; P1 reads it too (and was
             * recorded reading it, so P1 stays faithful), but on the LIVE record side P2 read the
             * FRESH s_netMerged (= this frame's merged word), one frame ahead of g_InputGameplay.
             * Reading g_InputGameplay for P2 therefore lagged it one frame behind its recorded self,
             * shifting every P2 rand-action by a frame -> the residual replay desync. The head word
             * (recorded[N]) is the replay equivalent of s_netMerged, so record and playback now drive
             * P2 from the identical frame. Falls back to g_InputGameplay if the head can't be read. */
            uint16_t p2in;
            if (IsReplayPlayback()) {
                void *rm = *ADDR_REPLAY_MGR;
                unsigned char *head = (recActive && rm) ? *(unsigned char **)((char *)rm + 0x84) : NULL;
                p2in = UnpackP2(head ? *(unsigned short *)head : *ADDR_INPUT_GAMEPLAY);
            } else if (s_netActive) {
                p2in = UnpackP2(s_netMerged);
            } else {
                p2in = s_p2LocalIn;        /* captured at the record seam — same word saved to .rpy */
            }
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
            float p2LivesBefore = s_p2Lives;   /* detect ZUN's on-death respawn */
            int p2char = ((s_p2Sel >= 0 ? s_p2Sel : *ADDR_SEL_ID) / 2) % 3;
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
            CRUMB2("update:P2", 1);
            SwapSelGlobals(1);              /* char/type-gated branches see P2 */
            SwapAnm(1);                     /* 0x400-range tables -> P2's char */
            s_origUpdate(p2);               /* trampoline → no detour re-entry */
            SwapAnm(0);
            SwapSelGlobals(0);
            s_inP2Update = 0;
            CRUMB2("update", 0);

            if (s_p2SepRes && rb) {
                s_p2Lives = *rl; s_p2Bombs = *rb; s_p2Power = *rp;  /* capture P2's */
                /* resolve the phantom: consumed -> last-life death -> ghost;
                 * untouched -> take it back (real spares stay 0) */
                if (p2Fake) {
                    if (s_p2Lives < 1.0f) { s_p2Lives = 0.f; EnterGhostP2(); }
                    else                    s_p2Lives -= 1.0f;
                }
                /* on a NORMAL respawn (a spare was just consumed, P2 still alive),
                 * ZUN refilled bombs to its global/config default (which tracks
                 * P1's character). Override to P2's OWN character default so each
                 * player keeps its canonical bomb stock. Ghost deaths are handled
                 * by the revive system (death-stock bombs), so skip those. */
                if (!s_p2Ghost && s_p2Lives < p2LivesBefore)
                    s_p2Bombs = kCharStartBombs[p2char];
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

            /* proximity transparency: fade the remote player when players overlap */
            if (s_proxFade && !s_p1Ghost && !s_p2Ghost && !s_runOver)
                ApplyProximityFade(p2);

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

    /* live netplay status (EoSD-style connection readout): role, sync state, ping,
     * and a stall warning. Tucked into the bottom-right corner just above ZUN's FPS
     * counter (drawn at 512,465) and shrunk to ~0.75x so it stays out of the way —
     * the playfield-bottom-left full-size line was distracting (user, §8af). The
     * verbose F#/delay fields were dev-only; a stall now shows as WAIT###ms.
     *
     * The ascii manager captures its current scale (mgr+0x74c4 = X, +0x74c8 = Y,
     * floats) into each text entry at print time, so save/shrink/print/restore
     * scales ONLY our line and leaves the FPS counter (same manager) untouched. */
    if (s_netActive || s_netPeerLost) {
        float npos[3];
        npos[0] = 496.f; npos[1] = 451.f; npos[2] = 0.f;   /* above the FPS readout */
        int ping = Nc_GetPing();   /* round-trip latency, ms (-1 = not yet measured) */
        volatile float *sx = (volatile float *)((char *)ADDR_ASCII_MGR + 0x74c4);
        volatile float *sy = (volatile float *)((char *)ADDR_ASCII_MGR + 0x74c8);
        float oldX = *sx, oldY = *sy;
        *sx = oldX * 0.75f; *sy = oldY * 0.75f;
        if (s_netPeerLost)
            ADDR_ASCII_PRINT(ADDR_ASCII_MGR, npos, "NET LOST");
        else if (s_netWaitMs >= 100)
            ADDR_ASCII_PRINT(ADDR_ASCII_MGR, npos, "NET %c WAIT%dms",
                             Nc_IsHost() ? 'H' : 'G', s_netWaitMs);
        else if (ping < 0)
            ADDR_ASCII_PRINT(ADDR_ASCII_MGR, npos, "NET %c %s",
                             Nc_IsHost() ? 'H' : 'G', s_netSync ? "SYNC" : "DSYN");
        else
            ADDR_ASCII_PRINT(ADDR_ASCII_MGR, npos, "NET %c %s %dms",
                             Nc_IsHost() ? 'H' : 'G', s_netSync ? "SYNC" : "DSYN", ping);
        *sx = oldX; *sy = oldY;   /* restore so the FPS counter stays full-size */
    }

    pos[1] = 296.f;
    if (s_p2Ghost)
        ADDR_ASCII_PRINT(ADDR_ASCII_MGR, pos, "P2 GHOST");
    else if (!s_p2IconHud) {
        /* legacy text readout — replaced by the icon HUD (HookedHudDraw) when ON */
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

/* A HUD icon sprite-object is only safe to blit if ZUN's own blit gate would
 * pass: FUN_0044f770 returns early unless draw-enable bits 0&1 (+0x1c0) are set
 * and the +0x1bb byte is nonzero, and it then DEREFERENCES the bound anm entry
 * (+0x1e4) -- so a zero/stale anm pointer there is a hard crash. We snapshot the
 * live object only when all of that holds, the same frame we replay it, so the
 * captured anm pointer is always current (no stale char-select / pre-reload ptr).
 * We copy ZUN's object and replay our copy; we NEVER poke ZUN's live object. */
static int IconValid(char *obj)
{
    return (*(unsigned *)(obj + SPR_OFF_FLAGS) & 3) == 3
        &&  obj[0x1bb] != 0
        &&  *(unsigned *)(obj + SPR_OFF_ANM) != 0;
}

/* Draw a row of `count` icons by re-blitting ZUN's live icon object `obj` at
 * (HUD_ICON_X + i*step, y) through the anm manager `mgr` (ecx). We save the
 * object's current pos/scale, blit each icon, then restore -- so P1's HUD is
 * untouched. FUN_0044f770 is no longer hooked, so calling it directly is safe. */
static void DrawIconRowLive(void *mgr, char *obj, int count, float y)
{
    float    ox = *(float *)(obj + HUD_SPR_X);
    float    oy = *(float *)(obj + HUD_SPR_Y);
    uint32_t oz = *(uint32_t *)(obj + HUD_SPR_Z);
    int i;
    if (count > HUD_ICON_MAX) count = HUD_ICON_MAX;
    for (i = 0; i < count; i++) {
        *(float *)(obj + HUD_SPR_X) = HUD_ICON_X + HUD_ICON_STEP * (float)i;
        *(float *)(obj + HUD_SPR_Y) = y;
        *(uint32_t *)(obj + HUD_SPR_Z) = HUD_ICON_SCALE;
        ADDR_SPRITE_BLIT(mgr, 0, obj);
    }
    *(float *)(obj + HUD_SPR_X) = ox;
    *(float *)(obj + HUD_SPR_Y) = oy;
    *(uint32_t *)(obj + HUD_SPR_Z) = oz;
}

/* §8a: P1-style icon HUD for P2. We append P2's life/bomb rows (from captured
 * templates) right after ZUN's HUD pass, then flush the shared sprite batch. */
static int s_p2HudClear = 0;   /* (reserved) frames to keep clearing after P2 stops */

static void __fastcall HookedHudDraw(void *self)
{
    void *p2  = (void *)s_p2;
    /* The life/bomb icon sprite objects live in the HUD object block at
     * *(self+8)+0x14ac / +0x16f8 (PCBdecomp FUN_0042b603 @17069/17085) -- NOT at
     * the score-pointer global. Reading the wrong base was why every capture came
     * back zeroed. */
    char *iconBase = (char *)*(uint32_t *)((char *)self + 8);
    int  drawP2 = (s_p2IconHud && p2 && !s_p2Ghost && iconBase != NULL);

    /* Keep ZUN's sidebar redraw-request live so the cached panel is cleared+
     * repainted every frame. Without this, P2's own per-frame text (power number
     * here, GHOST/REVIVE/SHARE in DrawCoopHud) smears on the stale panel. Raise
     * only -- never stomp a longer request ZUN may have set (e.g. pause). */
    if (p2 && *ADDR_HUD_REDRAW_REQ < 2)
        *ADDR_HUD_REDRAW_REQ = 2;

    s_origHudDraw(self);                      /* ZUN paints P1's full HUD */

    /* Snapshot the LIVE icon objects right AFTER ZUN drew P1's with them -- their
     * anm binding is guaranteed current this frame (no stale char-select ptr).
     * IconValid mirrors FUN_0044f770's own gate so a bad object is skipped, never
     * blitted (the blit dereferences the anm ptr). */
    if (drawP2) {
        void *mgr = (void *)*ADDR_SPRITE_MGR;   /* g_AnmManager -- ecx for the blit */
        char *ll = iconBase + HUD_OFF_LIFE_ICON;
        char *lb = iconBase + HUD_OFF_BOMB_ICON;
        int   lifeOK = mgr && IconValid(ll);
        int   bombOK = mgr && IconValid(lb);
        static int announced = 0;
        if (!announced) {
            Log("P2 icon HUD: mgr=%p base=%p lifeOK=%d bombOK=%d", mgr, (void *)iconBase, lifeOK, bombOK);
            Log("  life: flags=%08x b1bb=%02x anm=%08x | bomb: flags=%08x b1bb=%02x anm=%08x",
                *(unsigned *)(ll + SPR_OFF_FLAGS), (unsigned char)ll[0x1bb], *(unsigned *)(ll + SPR_OFF_ANM),
                *(unsigned *)(lb + SPR_OFF_FLAGS), (unsigned char)lb[0x1bb], *(unsigned *)(lb + SPR_OFF_ANM));
        }
        if (lifeOK) {
            DrawIconRowLive(mgr, ll, (int)s_p2Lives, HUD_P2_LIVES_Y);
            s_lifeReady = 1;
        }
        if (!announced) Log("  life row done");
        if (bombOK) {
            DrawIconRowLive(mgr, lb, (int)s_p2Bombs, HUD_P2_BOMBS_Y);
            s_bombReady = 1;
        }
        if (!announced) Log("  bomb row done");
        {   /* P2 power number via the ascii queue ("2P" label dropped per user) */
            float pos[3];
            pos[0] = HUD_ICON_X; pos[1] = HUD_P2_POWER_Y; pos[2] = 0.f;
            ADDR_ASCII_PRINT(ADDR_ASCII_MGR, pos, "%d", (int)s_p2Power);
        }
        if (!announced) Log("  ascii done");
        /* push our appended quads: ZUN's last flush already happened inside the
         * orig pass, so without this our quads sit in an unflushed batch. */
        if (mgr) ADDR_SPRITE_FLUSH(mgr);
        if (!announced) Log("  flush done");
        announced = 1;
    }
    (void)s_p2HudClear;
}

/* Redirect "angle toward the player" to whichever live player is nearer to the
 * firing/suction origin `pos` (or P2 when P1 is a ghost): we just hand the
 * original FUN_00442370 a different `this` (the P2 object). Covers enemy aimed
 * shots and item suction in one place. Passthrough when off / no P2 / P2 ghost. */
static long double __fastcall HookedAngleToPlayer(void *player, void *edx, float *pos)
{
    void *p2 = (void *)s_p2;
    if (s_coopAim && p2 && !s_p2Ghost && pos) {
        char *p1 = (char *)ADDR_PLAYER_BASE;
        float px = pos[0], py = pos[1];
        float ax = *(float *)(p1 + OFF_POS_X),          ay = *(float *)(p1 + OFF_POS_Y);
        float bx = *(float *)((char *)p2 + OFF_POS_X),  by = *(float *)((char *)p2 + OFF_POS_Y);
        float d1 = (ax - px) * (ax - px) + (ay - py) * (ay - py);
        float d2 = (bx - px) * (bx - px) + (by - py) * (by - py);
        if (s_p1Ghost || d2 < d1)
            return s_origAngleToPlayer(p2, edx, pos);
    }
    return s_origAngleToPlayer(player, edx, pos);
}

/* Replicate the enemy-update target acquisition (PCBdecomp.c 12904-12943) for P2,
 * relative to P2's OWN position + character, over this frame's enemy snapshot, and
 * write P2's +0x2428 block. Mirrors ZUN's logic: among bit6 (boss) enemies pick the
 * one nearest in X; SakuyaA additionally records the nearest in-cone enemy as the
 * aim target; popcorn (no bit6 target yet) falls back to the lowest enemy on screen
 * (homing) and any in-cone enemy (aim). Whatever P2 doesn't resolve itself (no
 * damageable enemy of that kind this frame) falls back to mirroring P1's block, so
 * this is never worse than the old full mirror. */
static void BuildP2TargetBlock(void *p2)
{
    char *pp = (char *)p2, *p1 = (char *)ADDR_PLAYER_BASE;
    float Px = *(float *)(pp + OFF_POS_X);
    float Py = *(float *)(pp + OFF_POS_Y);
    float Pz = *(float *)(pp + OFF_POS_Z);
    int sakuya = (((s_p2Sel >= 0 ? s_p2Sel : *ADDR_SEL_ID) / 2) % 3) == 2;
    float hX = -1000.f, hY = -1000.f, hZ = -1000.f;   /* homing target            */
    float aX = -1000.f, aY = -1000.f, aZ = -1000.f;   /* SakuyaA aim target       */
    int valid = 0;
    int i;
    (void)Pz;

    for (i = 0; i < s_enemyCount; i++) {
        char *en = (char *)s_enemySnap[i];
        unsigned char fl = *(unsigned char *)(en + ENEMY_OFF_FLAGS);
        float ex = *(float *)(en + ENEMY_OFF_POS);
        float ey = *(float *)(en + ENEMY_OFF_POS + 4);
        float ez = *(float *)(en + ENEMY_OFF_POS + 8);
        float dx = ex - Px,  dy = ey - Py;
        float adx = dx < 0.f ? -dx : dx;

        if (fl & ENEMY_FLAG_BOSS) {
            float ahx = hX - Px;  if (ahx < 0.f) ahx = -ahx;
            if (!valid || adx < ahx) { hX = ex; hY = ey; hZ = ez; }
            if (sakuya) {
                int inCone = (dy < 0.f) && (adx <= -dy * COOP_AIM_CONE_TAN30);
                float aax = aX - Px;  if (aax < 0.f) aax = -aax;
                if (inCone && (!valid || adx < aax)) { aX = ex; aY = ey; aZ = ez; valid = 1; }
            } else {
                valid = 1;
            }
        }
        if (!valid) {
            if (hY < ey) { hX = ex; hY = ey; hZ = ez; }   /* lowest enemy on screen */
            if (sakuya && aY < -900.f) {
                int inCone = (dy < 0.f) && (adx <= -dy * COOP_AIM_CONE_TAN30);
                if (inCone) { aX = ex; aY = ey; aZ = ez; }
            }
        }
    }

    /* No damageable enemy this frame (e.g. boss mid-invuln) -> P2 acquired nothing;
     * mirror P1's whole block, exactly the legacy behaviour (never worse). */
    if (s_enemyCount == 0) {
        memcpy(pp + OFF_HOMING_TGT, p1 + OFF_HOMING_TGT, HOMING_TGT_LEN);
        return;
    }
    /* homing: a non-empty snapshot always resolves one (bit6 nearest-x or the
     * lowest-enemy fallback); guard defensively and mirror P1 if somehow not. */
    if (hY > -900.f) {
        *(float *)(pp + OFF_HOMING_X)     = hX;
        *(float *)(pp + OFF_HOMING_X + 4) = hY;
        *(float *)(pp + OFF_HOMING_X + 8) = hZ;
    } else {
        memcpy(pp + OFF_HOMING_X, p1 + OFF_HOMING_X, 12);
    }
    /* aim: P2's own in-cone pick, else mirror P1's aim target (SakuyaA only reads
     * it; for other chars it stays sentinel/P1's and is unused). */
    if (aY > -900.f) {
        *(float *)(pp + OFF_AIM_X)     = aX;
        *(float *)(pp + OFF_AIM_X + 4) = aY;
        *(float *)(pp + OFF_AIM_X + 8) = aZ;
    } else {
        memcpy(pp + OFF_AIM_X, p1 + OFF_AIM_X, 12);
    }
    /* valid flag: engine-accurate — set only when a bit6 target locked (for Sakuya,
     * only with an in-cone enemy). The lowest-enemy/cone fallback leaves it 0 and
     * the consumer reads the coord sentinel instead. */
    *(int *)(pp + OFF_TGT_VALID) = valid;
}

static int __fastcall HookedDraw(void *self)
{
    CRUMB2("draw", 0);
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
            /* proximity fade of the REMOTE focus ring — applied HERE (draw phase,
             * after every update task incl. the effect anm that rewrites the colour)
             * so it survives to the draw, unlike the clobbered update-phase write. */
            if (s_proxFxPending) {
                s_proxFxPending = 0;
                void *fp = s_proxFxRemoteIsP2 ? p2 : (void *)ADDR_PLAYER_BASE;
                FadePlayerFocusFx(fp, s_proxFxTint);
                if (!s_proxFxLogged) {
                    uint32_t fx = *(uint32_t *)((char *)fp + OFF_PLAYER_FX);
                    Log("prox-fade: focus-ring tint at draw (fx=0x%08x type=%d tint=0x%08x)",
                        fx, fx ? *(unsigned char *)(fx + OFF_FX_TYPE) : -1, s_proxFxTint);
                    s_proxFxLogged = 1;
                }
            }
            /* feed P2 this frame's homing + SakuyaA aim targets. With per-player
             * aim on, acquire P2's own targets relative to ITS position (see
             * BuildP2TargetBlock); otherwise mirror P1's block (legacy). Consume
             * + clear the per-frame enemy snapshot either way. */
            if (s_perPlayerAim && !s_p2Ghost)
                BuildP2TargetBlock(p2);
            else
                memcpy((char *)p2 + OFF_HOMING_TGT,
                       (char *)ADDR_PLAYER_BASE + OFF_HOMING_TGT, HOMING_TGT_LEN);
            s_enemyCount = 0;
            CRUMB2("draw:P2", 1);
            SwapSelGlobals(1);
            SwapAnm(1);                     /* P2's char sprites for its draw */
            s_origDraw(p2);
            SwapAnm(0);
            SwapSelGlobals(0);
            CRUMB2("draw", 0);
            DrawCoopHud(p2);
        }
    }
    return r;
}

/* ---- menu character-select hook (handoff §5g) ---- */

/* Full coop teardown for a return to the front-end menu. Unlike DespawnP2 (which
 * runs in-stage and pops P2's focus ring through the engine), the ended game's
 * engine objects — the focus-FX effect, the effect array, the stage anms — are
 * all gone or recycled by now, so we must NOT dereference any of them: we just
 * DROP the stale handles, free our own clone buffer + the anm slot, and clear
 * every session latch back to its pristine pre-game state. Players reset and
 * re-enter character select constantly, so each new game must start clean — a
 * surviving clone froze the 2nd game's stage load. */
static void ResetCoopSession(void)
{
    void *p2 = (void *)s_p2;
    s_p2 = NULL;                 /* stop any piggyback FIRST                       */
    s_p2FocusFx = NULL;          /* DROP the stale fx handle — never KillP2FocusFx */
    if (s_p2AnmSlot >= 0) { SwapAnm(0); FreeP2CharAnm(); }
    if (p2) VirtualFree(p2, 0, MEM_RELEASE);
    s_enemyCount    = 0;         /* drop the enemy snapshot (stale ptrs)          */
    s_autoSpawned   = 0;
    s_readyFrames   = 0;
    s_replayIsCoop  = 0;         /* §8k: forget the last replay's co-op tag        */
    s_replayNetRecorded = 0;     /* §8aa: forget last replay's net-recorded/FPU flag */
    s_rpyStageBlk = NULL;        /* §8ah: forget last recording's stage block (seed backfill target) */
    s_rpySeedLogged = 0;         /* §8ah: re-log the stage-seed fix for the next recording */
    s_replaySeedArm = 0;         /* §8ai: cancel a pending first-in-stage seed re-force */
    s_replayInitSeed = 0;        /* §8ai */
    s_replayPerFrameSeed = 0;    /* §8aj: re-decided at the next replay load            */
    s_rpySeedStored = 0;         /* §8aj: re-log per-frame seed capture for the next recording */
    s_rpySeedRestored = 0;       /* §8aj: re-log per-frame seed restore for the next playback  */
    s_rngHistN = 0;              /* §8am: drop any partial RNG-caller histogram         */
    s_rngHistRf = -1; s_rngHistPlay = -1;
    s_replayTargetCw = 0x007f;   /* §8ad: default precision until the next replay loads its cw */
    s_replayPinCw = 0;           /* §8ad: no sim-seam pin until the next replay decides */
    s_fpuPinnedReplay = 0;       /* re-log the replay FPU pin for the next replay    */
    s_replayPinDecisionLogged = 0;
    s_replayLoadLogged = 0;      /* so the next replay re-logs its P2 character     */
    s_p1Ghost       = 0;
    s_p2Ghost       = 0;
    s_runOver       = 0;
    s_reviveFrames  = 0;
    s_shareFrames   = 0;
    s_p2Carry       = 0;
    s_p2Shadow      = 0;
    s_p2HudClear    = 0;
    s_lifeReady     = 0;   /* re-capture icon templates next stage (front.anm may reload) */
    s_bombReady     = 0;
    s_lastLogicFrame = 0x7fffffff;
    Log("coop session reset (returned to front-end menu)");
}

/* P2's menu input in the MENU bit layout (same configurable binds as
 * ReadP2InputLocal: move = dpad, SHOOT = confirm, BOMB = cancel). */
static uint16_t ReadP2MenuInput(void)
{
    uint16_t w = 0;
    if (Down(s_p2Keys[P2K_UP]))    w |= 0x10;          /* up    */
    if (Down(s_p2Keys[P2K_DOWN]))  w |= 0x20;          /* down  */
    if (Down(s_p2Keys[P2K_LEFT]))  w |= 0x40;          /* left  */
    if (Down(s_p2Keys[P2K_RIGHT])) w |= 0x80;          /* right */
    if (Down(s_p2Keys[P2K_SHOOT])) w |= MENU_CONFIRM;  /* confirm */
    if (Down(s_p2Keys[P2K_BOMB]))  w |= MENU_CANCEL;   /* cancel  */
    return w;
}

/* Inline the engine's screen-transition setter FUN_00455435(menu, state): stash
 * the old state, set the new one, and reset the per-screen counters + substate so
 * the target screen re-runs its first-frame setup. */
static void MenuGotoState(char *menu, uint32_t state)
{
    *(uint32_t *)(menu + MENU_OFF_PREVSTATE) = *(uint32_t *)(menu + MENU_OFF_STATE);
    *(uint32_t *)(menu + MENU_OFF_STATE)     = state;
    *(uint32_t *)(menu + MENU_OFF_COUNTER3)  = 0;
    *(uint32_t *)(menu + MENU_OFF_FRAMECTR)  = 0;
    *(uint32_t *)(menu + MENU_OFF_SUBSTATE)  = 0;
    *(uint32_t *)(menu + MENU_OFF_COUNTER2)  = 0;
}

static int MenuIsCharState(uint32_t s) { return s == 5 || s == 0xd; }
static int MenuIsShotState(uint32_t s) { return s == 6 || s == 0xe; }
static int MenuInCoopFlow(uint32_t s)
{ return s == 4 || s == 5 || s == 6 || s == 0xc || s == 0xd || s == 0xe; }

/* Menu netplay status line (top row). The menu is just as easy to DESYNC as a
 * stage — HookedSceneTick runs the same lockstep merge on the front-end, so
 * s_netFrame / s_netSync / the RNG oracle (self vs rcv) are all live here. The
 * menu scene flushes the global ascii queue every frame (same path as the "P2
 * SELECT CHARACTER" prompt), so this is a pure draw — no determinism impact.
 * Shows role, lockstep frame, delay, sync state, the per-frame wait when it
 * climbs (a stall), and the self/rcv RNG seeds so a menu desync is visible the
 * instant the two seeds diverge. */
#define MENU_NET_X   8.f
#define MENU_NET_Y   12.f
static void DrawNetMenuStatus(void)
{
    float pos[3];
    pos[0] = MENU_NET_X; pos[1] = MENU_NET_Y; pos[2] = 0.f;
    if (s_netPeerLost) {
        ADDR_ASCII_PRINT(ADDR_ASCII_MGR, pos, "NET LOST");
    } else if (s_netActive) {
        if (s_netWaitMs >= 100)
            ADDR_ASCII_PRINT(ADDR_ASCII_MGR, pos, "NET %c F%d WAIT%dms",
                             Nc_IsHost() ? 'H' : 'G', s_netFrame, s_netWaitMs);
        else {
            int ping = Nc_GetPing();   /* item 3: ping while waiting to connect */
            if (ping < 0)
                ADDR_ASCII_PRINT(ADDR_ASCII_MGR, pos, "NET %c F%d d%d %s %04X/%04X PING--",
                                 Nc_IsHost() ? 'H' : 'G', s_netFrame, Nc_GetDelay(),
                                 s_netSync ? "SYNC" : "DSYN", s_netSelfRng, s_netRcvRng);
            else
                ADDR_ASCII_PRINT(ADDR_ASCII_MGR, pos, "NET %c F%d d%d %s %04X/%04X PING%dms",
                                 Nc_IsHost() ? 'H' : 'G', s_netFrame, Nc_GetDelay(),
                                 s_netSync ? "SYNC" : "DSYN", s_netSelfRng, s_netRcvRng, ping);
        }
    }
}

static int __fastcall HookedMenuDispatch(void *menuv)
{
    char *menu = (char *)menuv;
    uint32_t state = *(uint32_t *)(menu + MENU_OFF_STATE);
    int r;

    /* surface the netplay sync state on the front-end too (top row) — a menu
     * desync is otherwise invisible until the game won't start in sync */
    if (s_netActive || s_netPeerLost) DrawNetMenuStatus();

    /* The menu dispatcher ONLY runs in the front-end (title/main/char-select),
     * never during gameplay. So if a co-op session is still alive here, the
     * previous game ended/quit without an in-stage teardown — fully reset it now
     * so (a) the char-select portraits don't glyph from a leftover anm slot and
     * (b) a stale clone can't survive into the next game's stage load (which
     * froze the 2nd run). Idempotent: after the reset s_p2/s_p2AnmSlot are clear. */
    if (s_p2 || s_p2AnmSlot >= 0)
        ResetCoopSession();

    /* Under netplay the SAME two-pass FSM runs, but each pass is driven by that
     * player's DE-MERGED wire word (s_netP1Menu / s_netP2Menu from Nc_GetLastSplit)
     * instead of the local keyboard — deterministic on both machines, so P2 picks
     * its OWN character. Local play uses the keyboard path exactly as before. */
    if (!s_menuSelect) return s_origMenuDispatch(menuv);
    int net = s_netActive;

    /* left the char/shot-select flow (back to title/difficulty, or post-game):
     * reset the FSM so the next game starts a fresh P2 selection. */
    if (!MenuInCoopFlow(state)) {
        if (s_coopMenu != CM_IDLE) {
            Log("coop-menu: left select flow (state %u) -> reset", state);
            s_coopMenu = CM_IDLE;
        }
        return s_origMenuDispatch(menuv);
    }

    /* ---- P1's pass: intercept the shot-type COMMIT, divert to P2 ---- */
    if (s_coopMenu == CM_IDLE) {
        /* netplay: isolate P1's pass to P1's own wire word — else P2's bits, OR'd
         * into g_InputMenu by HookedSceneTick, would also move P1's cursor. */
        uint16_t saveCur = 0, savePrev = 0;
        if (net) {
            saveCur = *ADDR_MENU_IN_CUR; savePrev = *ADDR_MENU_IN_PREV;
            *ADDR_MENU_IN_CUR = s_netP1Menu; *ADDR_MENU_IN_PREV = s_p1MenuPrev;
        }
        if (MenuIsShotState(state)) {
            int substate    = *(int *)(menu + MENU_OFF_SUBSTATE);
            uint16_t cur     = *ADDR_MENU_IN_CUR, prev = *ADDR_MENU_IN_PREV;
            int confirmEdge  = (cur & MENU_CONFIRM) &&
                               ((cur & MENU_CONFIRM) != (prev & MENU_CONFIRM));
            if (substate == 1 && confirmEdge && (*ADDR_MODE_FLAGS & 1) == 0) {
                /* P1 just locked its shot type — orig would start the game now.
                 * Capture P1's pick and send the menu back to char-select for P2. */
                s_p1Char = *ADDR_CHAR_ID;     /* set during P1's char-select pass   */
                s_p1Type = (int)*(uint32_t *)(menu + MENU_OFF_CURSOR); /* shot cursor */
                if (s_p1Type < 0 || s_p1Type > 1) s_p1Type = 0;
                *ADDR_TYPE_ID = (unsigned char)s_p1Type;   /* what orig would store  */
                s_p2Sel = -1;                 /* fresh P2 selection for this game    */
                s_p2FaceDiagDone = -1;        /* re-run the §8b face probe this game */
                s_allowDiffChar = 0;
                MenuGotoState(menu, state - 1);            /* 6->5, 0xe->0xd          */
                s_coopMenu = CM_P2_CHAR;
                /* seed P2's prev with currently-held input so a held confirm reads
                 * as already-down on P2's first pass, not a fresh edge */
                s_menuPrev = net ? s_netP2Menu
                                 : (uint16_t)(*ADDR_MENU_IN_CUR | ReadP2MenuInput());
                Log("coop-menu%s: P1 locked %s -> P2 character select",
                    net ? "(net)" : "", ADDR_SEL_NAMES[s_p1Char * 2 + s_p1Type]);
            }
        }
        r = s_origMenuDispatch(menuv);
        if (net) {
            *ADDR_MENU_IN_CUR = saveCur; *ADDR_MENU_IN_PREV = savePrev;
            s_p1MenuPrev = s_netP1Menu;
        }
        return r;
    }

    /* ---- P2's pass: route P2 (and P1, as a helper) input into the menu ---- */
    if (s_coopMenu == CM_P2_CHAR || s_coopMenu == CM_P2_SHOT) {
        /* on-screen cue that it's P2's turn (handoff §8d) — queued every frame on
         * the global ascii manager, which the menu scene also flushes */
        float ppos[3];
        ppos[0] = MENU_PROMPT_X; ppos[1] = MENU_PROMPT_Y; ppos[2] = 0.f;
        ADDR_ASCII_PRINT(ADDR_ASCII_MGR, ppos,
                         s_coopMenu == CM_P2_CHAR ? "P2 SELECT CHARACTER"
                                                  : "P2 SELECT SHOT");

        uint16_t realCur  = *ADDR_MENU_IN_CUR;
        uint16_t realPrev = *ADDR_MENU_IN_PREV;
        /* netplay: P2's OWN de-merged wire word (isolated). Local: P1's keyboard
         * OR P2's keys, so either player can drive P2's pick on one keyboard. */
        uint16_t combined = net ? s_netP2Menu
                                : (uint16_t)(realCur | ReadP2MenuInput());

        *ADDR_MENU_IN_PREV = s_menuPrev;
        *ADDR_MENU_IN_CUR  = combined;
        r = s_origMenuDispatch(menuv);
        *ADDR_MENU_IN_CUR  = realCur;
        *ADDR_MENU_IN_PREV = realPrev;
        s_menuPrev = combined;

        {
            uint32_t newState = *(uint32_t *)(menu + MENU_OFF_STATE);
            if (s_coopMenu == CM_P2_CHAR && MenuIsShotState(newState)) {
                s_coopMenu = CM_P2_SHOT;                /* P2 confirmed its char     */
                Log("coop-menu: P2 char=%d -> shot select", *ADDR_CHAR_ID);
            } else if (s_coopMenu == CM_P2_SHOT && MenuIsCharState(newState)) {
                s_coopMenu = CM_P2_CHAR;                /* P2 cancelled back to char */
            } else if (s_coopMenu == CM_P2_SHOT && r == 0) {
                /* P2's shot-type COMMIT (dispatcher returns 0 only on the start
                 * path): orig set 645/646 to P2's pick and began the stage start.
                 * Record P2's selection, then restore P1's globals so the engine
                 * inits the static player as P1 (player init reads them later, at
                 * the game scene — async from this drain). */
                int p2char = *ADDR_CHAR_ID;
                int p2type = *ADDR_TYPE_ID;
                s_p2Sel = p2char * 2 + p2type;
                s_allowDiffChar = (p2char != s_p1Char) ? 1 : 0;
                *ADDR_CHAR_ID = (unsigned char)s_p1Char;
                *ADDR_TYPE_ID = (unsigned char)s_p1Type;
                *ADDR_SEL_ID  = (unsigned char)(s_p1Char * 2 + s_p1Type);
                s_coopMenu = CM_COMMIT;
                Log("coop-menu: P2 locked %s; P1=%s -> start (p2Sel=%d diff=%d)",
                    ADDR_SEL_NAMES[s_p2Sel], ADDR_SEL_NAMES[s_p1Char * 2 + s_p1Type],
                    s_p2Sel, s_allowDiffChar);
            }
        }
        return r;
    }

    return s_origMenuDispatch(menuv);
}

/* Disable the title-screen demo (attract mode). The menu update at 0x004559xx
 * counts idle frames in menuObj+0xd100 and, once `900 < idleFrames`, loads
 * data/demo/demorpyN.rpy — which advances game state + RNG. That's pure noise
 * solo, and actively harmful under netplay: a player idling at the title waiting
 * for their peer would kick off a demo. We raise the 900 threshold to ~INT_MAX so
 * it never fires (the same fix the EoSD co-op mod uses). The instruction
 * `CMP [EAX+0xd100], 0x384` (81 b8 00 d1 00 00 84 03 00 00) is at 0x00455a94, so
 * its imm32 lives at 0x00455a94+6 = 0x00455a9a (NOT ...9c — that lands on the high
 * half of the imm + the following 0f 8e JLE, which is why the first cut read back
 * 0x8e0f0000 and skipped). We only patch if it still reads 900, so a wrong build is
 * left untouched. */
#define ADDR_DEMO_THRESHOLD_IMM ((void *)0x00455a9a)
static void PatchDisableDemo(void)
{
    void *p = ADDR_DEMO_THRESHOLD_IMM;
    DWORD old;
    if (!VirtualProtect(p, 4, PAGE_EXECUTE_READWRITE, &old)) {
        Log("demo-play patch FAILED: VirtualProtect err=%lu", GetLastError());
        return;
    }
    if (*(uint32_t *)p == 0x00000384u) {
        *(uint32_t *)p = 0x7FFFFFF8u;
        Log("demo-play disabled (idle threshold 900 -> INT_MAX at 0x00455a9a)");
    } else {
        Log("demo-play patch SKIPPED: bytes at 0x00455a9a = 0x%08x, not 900 (wrong build?)",
            *(uint32_t *)p);
    }
    VirtualProtect(p, 4, old, &old);
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
    if (MH_CreateHook(ADDR_ADD_CLEAR_CIRCLE,(LPVOID)&HookedAddClearCircle,(LPVOID*)&s_origAddClearCircle)!= MH_OK) return 0; /* B4 */
    if (MH_CreateHook(ADDR_ADD_CLEAR_BOX,  (LPVOID)&HookedAddClearBox,   (LPVOID*)&s_origAddClearBox)   != MH_OK) return 0; /* B4 */
    if (MH_CreateHook(ADDR_MENU_DISPATCH,  (LPVOID)&HookedMenuDispatch,  (LPVOID*)&s_origMenuDispatch)  != MH_OK) return 0;
    if (MH_CreateHook(ADDR_HUD_DRAW,       (LPVOID)&HookedHudDraw,       (LPVOID*)&s_origHudDraw)       != MH_OK) return 0;
    if (MH_CreateHook((LPVOID)ADDR_ANGLE_TO_PLAYER, (LPVOID)&HookedAngleToPlayer, (LPVOID*)&s_origAngleToPlayer) != MH_OK) return 0;
    if (MH_CreateHook(ADDR_DECL_MAKE,      (LPVOID)&HookedDeclMake,      (LPVOID*)&s_origDeclMake)      != MH_OK) return 0;
    if (MH_CreateHook(ADDR_DECL_DRAW,      (LPVOID)&HookedDeclDraw,      (LPVOID*)&s_origDeclDraw)      != MH_OK) return 0;
    if (MH_CreateHook(ADDR_REPLAY_HDR_INIT,(LPVOID)&HookedReplayHdrInit, (LPVOID*)&s_origReplayHdr)     != MH_OK) return 0;
    if (MH_CreateHook(ADDR_REPLAY_LOAD,    (LPVOID)&HookedReplayLoad,    (LPVOID*)&s_origReplayLoad)    != MH_OK) return 0; /* §8k playback */
    if (MH_CreateHook(ADDR_REPLAY_PLAY_TASK,(LPVOID)&HookedPlayTask,     (LPVOID*)&s_origPlayTask)      != MH_OK) return 0; /* §8ad playback FPU pin */
    if (MH_CreateHook(ADDR_RNG_FN,         (LPVOID)&HookedRng,           (LPVOID*)&s_origRng)           != MH_OK) return 0; /* §8al RNG-caller trace (gated by replay_trace) */
    /* B5: capture the enemy-manager base (FUN_00420620's ECX/param_1) so the boss-spell
     * armour gate reads the LIVE spellcardInfo.isActive at param_1+0x9545c8. The base is
     * non-zero in-game (the absolute-globals assumption was wrong — see HookedEnemyUpdate). */
    if (MH_CreateHook(ADDR_ENEMY_UPDATE,   (LPVOID)&HookedEnemyUpdate,   (LPVOID*)&s_origEnemyUpdate)   != MH_OK) return 0;
    /* Netplay seams: install ONLY when netplay is enabled in coop.ini. These two
     * addresses are unverified in-game, so with [net] enabled=0 (default) we don't
     * even lay the trampolines — the confirmed-good local build is byte-for-byte
     * the prior main. They're installed+enabled together below under the same gate. */
    if (s_netEnabled) {
        if (MH_CreateHook(ADDR_SCENE_TICK, (LPVOID)&HookedSceneTick, (LPVOID*)&s_origSceneTick) != MH_OK) return 0;
        if (MH_CreateHook(ADDR_GAME_START, (LPVOID)&HookedGameStart, (LPVOID*)&s_origGameStart) != MH_OK) return 0;
        /* §8ag: Extra/Phantasm unlock predicates — forced "unlocked" under netplay (see hooks). */
        if (MH_CreateHook(ADDR_EXTRA_UNLOCKED, (LPVOID)&HookedExtraUnlocked, (LPVOID*)&s_origExtraUnlocked) != MH_OK) return 0;
        if (MH_CreateHook(ADDR_PHANT_UNLOCKED, (LPVOID)&HookedPhantUnlocked, (LPVOID*)&s_origPhantUnlocked) != MH_OK) return 0;
    }
    /* B2: FP-safe naked detour on the item spawner, gated on coop.ini so a default
     * build is byte-for-byte unchanged. Create+enable together (MinHook allows it). */
    if (s_b2CherryBothFull) {
        if (MH_CreateHook((LPVOID)0x004326f0, (LPVOID)&HookedItemSpawn,
                          (LPVOID*)&g_b2OrigItemSpawn) != MH_OK) return 0;
        if (MH_EnableHook((LPVOID)0x004326f0) != MH_OK) return 0;
        Log("B2: power->cherry gated on BOTH players full (cherry_both_full=1)");
    }
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
    if (MH_EnableHook(ADDR_MENU_DISPATCH)  != MH_OK) return 0;
    if (MH_EnableHook(ADDR_HUD_DRAW)       != MH_OK) return 0;
    if (MH_EnableHook((LPVOID)ADDR_ANGLE_TO_PLAYER) != MH_OK) return 0;
    if (MH_EnableHook(ADDR_DECL_MAKE)      != MH_OK) return 0;
    if (MH_EnableHook(ADDR_DECL_DRAW)      != MH_OK) return 0;
    if (MH_EnableHook(ADDR_REPLAY_HDR_INIT)!= MH_OK) return 0;
    if (MH_EnableHook(ADDR_REPLAY_PLAY_TASK)!= MH_OK) return 0;  /* §8ad playback FPU pin */
    if (MH_EnableHook(ADDR_RNG_FN)         != MH_OK) return 0;   /* §8al RNG-caller trace */
    if (MH_EnableHook(ADDR_REPLAY_LOAD)    != MH_OK) return 0;   /* §8k: was CREATED but never
                                                                   ENABLED — playback ran unhooked,
                                                                   so co-op replays never spawned P2 */
    if (MH_EnableHook(ADDR_ENEMY_UPDATE)   != MH_OK) return 0;   /* B5: live enemy-manager base */
    if (s_netEnabled) {
        if (MH_EnableHook(ADDR_SCENE_TICK) != MH_OK) return 0;
        if (MH_EnableHook(ADDR_GAME_START) != MH_OK) return 0;
        if (MH_EnableHook(ADDR_EXTRA_UNLOCKED) != MH_OK) return 0;  /* §8ag unlocks */
        if (MH_EnableHook(ADDR_PHANT_UNLOCKED) != MH_OK) return 0;
    }
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
        AddVectoredExceptionHandler(1, CoopCrashHandler);   /* dump context on a crash */
        Log("th07_coop attached. Menu char-select: P1 picks, then P2 picks with "
            "WASD move / Space confirm / O cancel; game starts after both. AUTO-spawn "
            "P2 ~3s into the stage. In-stage P2: WASD move, Space shot, U focus, O bomb. "
            "F2=cycle P2 char, F3=toggle P2 type, F4=team-border, F5=boss-HP-scale, "
            "F6=sep-resources, F7=shot-damage, F8=killable, F9=spawn, F10=despawn, "
            "F11=revive, F12=HUD-style(icons/text).");
        Log("*** DIAGNOSTIC BUILD v9: the residual desync is INTERMITTENT (fpu_guard gave one "
            "bit-perfect run then desync recurred, with/without P2) — the tell that D3D leaves the "
            "x87 PRECISION control word in different states per machine/run (§8q). This build now "
            "PINS the x87 control word to 24-bit/round-nearest each frame under netplay (vanilla's "
            "value) so both machines agree — the likely real fix. fpu_guard (FPU firewall) stays on "
            "by default as a complement; fpucw/fpusw still logged. Watch the 'control word pinned "
            "(was 0x__)' line: if host and guest differ, that was the desync. Also B5 (P2 bomb -> "
            "Extra/Phantasm boss invuln). P2 binds: WASD. ***");
        LoadNetConfig();
        /* Echo the resolved config so every log self-documents what was actually read
         * (a hand-added diagnostic flag that lands in the wrong coop.ini copy silently
         * defaults off — this line makes that visible at a glance / lets the friend
         * pre-flight check solo before the netplay round). */
        Log("CONFIG (read from %scoop.ini): suppress_p2=%d  cherry_both_full=%d  "
            "fpu_guard=%d  auto_resync=%d  proximity_fade=%d  damper_boss_only=%d | net.enabled=%d role=%s delay=%d seed=0x%04x",
            s_dir, s_suppressP2, s_b2CherryBothFull, s_fpuGuard, s_netAutoResync, s_proxFade, s_damperBossOnly,
            s_netEnabled, s_netIsHost ? "host" : "guest", s_netDelay, s_netSeed);
        if (s_disableDemo) PatchDisableDemo();   /* kill title attract-mode demo */
        StartNet();        /* no-op unless coop.ini [net] enabled=1 */
        if (!InstallHooks())
            MessageBoxA(NULL, "th07_coop: hook install failed (wrong build/addresses?)",
                        "th07_coop", MB_ICONERROR);
        else
            Log("hooks installed (update @0x441fb0, draw @0x4420b0, "
                "collide-bullet @0x43e260, collide-laser @0x43e6b0, graze @0x43e3b0, "
                "damage @0x43d9e0, collect-overlap @0x43e4e0, item-loop @0x432990, "
                "border-break @0x441bd0, hud-draw @0x42b603)");
        break;
    case DLL_PROCESS_DETACH:
        if (s_trace) { fclose(s_trace); s_trace = NULL; }
        if (s_log) { Log("detach"); fclose(s_log); }
        MH_Uninitialize();
        break;
    }
    return TRUE;
}
