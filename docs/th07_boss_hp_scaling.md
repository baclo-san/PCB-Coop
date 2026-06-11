# Tier 1 — Boss / enemy HP scaling (verified against PCBdecomp.c)

> **STATUS (2026-06-11): IMPLEMENTED (Option A) in `src/coop/coop.c`** — the
> `HookedEclInterp` detour on `FUN_00424290` scales `+0xd30` by the active
> player count (`1 + P2-present`), auto-armed while P2 is live, F5 to toggle.
> Compile-verified; **not yet game-tested** (verify a boss takes ~2× hits with
> P2 spawned, and that the life bar drains at the correct rate). The static
> binary patch (Option B) was not pursued — the runtime detour supports 3P and
> needs no instruction-offset surgery.

**Goal (handoff §4 "Difficulty tiers"):** with two players the team does ~2× DPS,
so bosses die twice as fast. Scaling boss HP by the player count restores the
intended fight length. This is the *easiest* co-op gameplay change — a single
value, no new entities.

All offsets below were read directly from `PCBdecomp.c` and spot-verified.
Build-specific to th07.exe ver 1.00b (SHA256 `35467EAF…E80CA`).

---

## 1. The HP model is INVERTED — damage accumulates UP toward a cap

Bosses/enemies in PCB don't count HP down. A spell card / life phase stores a
**max-life threshold** and a **running damage accumulator**; the phase ends when
accumulated damage reaches the threshold.

Enemy-struct offsets (relative to the enemy object base):

| Offset | Type | Meaning |
|---|---|---|
| `+0xd18` | int | **accumulated damage** this phase (starts 0, climbs) |
| `+0xd14` | int/float | fractional-damage carry for `+0xd18` |
| `+0xd30` | int* | **max life of the phase (the scale target)** |
| `+0xd34` | int | phase / timeout count |
| `+0xd38` | int | phases elapsed |
| `+0xbb0` | float | full HP-bar width (for the on-screen bar) |
| `+0xbf4` | u16 | flags (bit cleared when last phase ends) |

\* `+0xd30` is *stored* via a `float` write at the init site but *read* as `int`
everywhere it matters (death check `*(int*)`, and bar math casts `(float)*(int*)`).
It is effectively an integer HP count — scale it as an integer.

### Death / phase-end condition (verified)
`FUN_00425400` (PCBdecomp.c:14456), and identically `FUN_00425580` (14489) and
`FUN_00425700` (14524) — the three life-bar advance/draw functions:

```c
if (*(int *)(param_1 + 0xd18) < *(int *)(param_1 + 0xd30)) {
    // still alive: bar fill = bb0 - ((d18 + d14) * bb0) / d30
} else {
    // phase over: advance phase, clear life-bar flag, reset d18=d14=0
}
```
So **doubling `+0xd30` doubles the damage needed to clear the phase** and the bar
drains at half the rate (the `/d30` in the fill math keeps it visually correct).

---

## 2. Where `+0xd30` is set — the patch site

`FUN_00424290` (PCBdecomp.c:14007), `__fastcall(int param_1)` — the per-enemy ECL
command interpreter. The "set life" opcode case (lines 14052–14069):

```c
*(float *)(param_1 + 0xd20) = *pfVar1;
...
*(float *)(param_1 + 0xd1c) = local_28;
*(undefined4 *)(param_1 + 0xd18) = 0;      // reset accumulated damage
*(undefined4 *)(param_1 + 0xd14) = 0;
*(undefined4 *)(param_1 + 0xd10) = 0xfffffc19;
*(float *)(param_1 + 0xd30) = pfVar1[2];   // <-- MAX LIFE set from ECL arg
*(float *)(param_1 + 0xd34) = pfVar1[3];   // timeout/phase count
```
`pfVar1` is the ECL command's argument array; `pfVar1[2]` is the life value.
This case runs once per spell card / life phase as the boss script declares it.

---

## 3. Recommended scaling implementations

### Option A (runtime DLL, recommended) — detour `FUN_00424290`, scale on change
Watch `+0xd30` across the original call; when it just got (re)written, multiply it
by the player count. Self-arming, survives every phase transition, no per-frame work.

```c
#define ADDR_ECL_INTERP ((LPVOID)0x00424290)   // FUN_00424290 __fastcall(enemy)
#define OFF_HP_MAX  0xd30
#define OFF_HP_ACC  0xd18
static int g_players = 2;                       // scale factor

static EclFn_t orig_424290;
int __fastcall Hook_424290(int enemy) {
    int before = *(int*)(enemy + OFF_HP_MAX);
    int r = orig_424290(enemy);
    int after  = *(int*)(enemy + OFF_HP_MAX);
    // life just (re)initialised this command, and accumulator was reset to 0:
    if (after != before && after > 0 && *(int*)(enemy + OFF_HP_ACC) == 0)
        *(int*)(enemy + OFF_HP_MAX) = after * g_players;
    return r;
}
```
Notes:
- Scaling only on the frame `+0xd30` *changes* avoids re-multiplying every frame.
- Guard on `+0xd18 == 0` (accumulator just reset) so we only scale a fresh phase,
  not an unrelated `+0xd30` touch.
- `g_players` should come from the co-op session (2 or 3). Under the netcode it is
  the connected player count; standalone it can be a config/hotkey.

### Option B (static binary patch) — double the stored value at line 14066
At the machine instruction that stores `pfVar1[2]` into `[enemy+0xd30]`, insert a
shift-left-1 (×2) on the value before the store. Smaller footprint, but hard-codes
the factor (no 3-player support) and needs the exact instruction offset. Prefer A.

### DO NOT hook `FUN_0043958d`
It looks tempting (it's the call right after the death check, `FUN_0043958d(d18,d14)`),
but it is a **generic counter-toward-target interpolation helper** used in ~50
unrelated places (player/enemy/item/effect counters; see the grep in the session
notes). Hooking it would corrupt half the game. Scale `+0xd30`, not the accumulator.

---

## 4. Open questions for a future session
- Confirm the *actual damage-application* path (where a player shot adds to `+0xd18`
  for a hit). The three `FUN_004254xx` functions only *advance/draw* the bar; the
  damage events feed `+0xd18` from the shot↔enemy collision. Not required for
  scaling (scaling the cap is sufficient), but needed if per-player damage tuning is
  ever wanted.
- Non-boss enemies use the same `FUN_00424290` interpreter; the same set-life opcode
  gives them HP too. Option A scales *all* enemies, which is usually desirable for
  co-op. If only bosses should scale, gate on the life-bar flag (`+0xbf4`) or a boss
  marker.
- Enemy array base/stride (agent-reported, NOT independently verified here):
  base `DAT_0063b218`, stride `0xd68`, state at `+0xbfc`. Verify before relying on a
  whole-array sweep; the detour approach above doesn't need them.
