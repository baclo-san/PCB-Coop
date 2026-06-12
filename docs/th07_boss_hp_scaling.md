# Tier 1 — Boss / enemy HP scaling (verified against PCBdecomp.c)

> **STATUS (2026-06-12): re-implemented on the DAMAGE side (§5), the ECL-cap
> detour (§2–§3) is RETIRED.** The game test + the per-cap-write `eclhp`
> diagnostic settled it:
> - Cirno (stage-1 midboss) kill time was unchanged with the ECL-cap detour
>   armed, and **every single eclhp line in the diagnostic run was
>   `cap 0 -> 120` (popcorn)** — the midboss's HP **never passes the
>   `FUN_00424290` set-life path** this doc's §2 pinned. Where boss/midboss HP
>   is actually set remains un-RE'd (a different opcode/handler); not pursued.
> - `coop.c` now detours **`FUN_0043d9e0`** (§5): the original runs (shot
>   consumption + sparks happen once), then only the returned damage is divided
>   by the player count, floored at 1. This catches every enemy uniformly no
>   matter how its HP was initialized. Auto-armed while P2 is live; F5 toggles.
> - Side note: the eclhp log burst (~40 lines in one frame at wave spawns, each
>   fflush'd) likely caused the brief freeze the user saw — the diagnostic is
>   removed with the switch.
>
> **GAME TEST round 3 (2026-06-12): PASSES for the boss** — Cirno nearly timed
> out with P2 dormant (~2× TTK, as intended). **Known accepted trade-off:**
> popcorn fairies now take TWO homing amulets instead of one (the integer
> halving — a fairy that died to one amulet's damage now needs two). User:
> shelf it for now. If it ever matters: gate the divisor on the target (only
> the boss-object path / life-bar flag `+0xbf4`), or round-up the division.

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
- ~~Confirm the *actual damage-application* path~~ ✅ **RESOLVED (2026-06-12
  overnight session)** — see §5: damage is computed by `FUN_0043d9e0` and applied
  by its caller, the per-enemy update `FUN_00420620` (PCBdecomp.c:12822).
- Non-boss enemies use the same `FUN_00424290` interpreter; the same set-life opcode
  gives them HP too. Option A scales *all* enemies, which is usually desirable for
  co-op. If only bosses should scale, gate on the life-bar flag (`+0xbf4`) or a boss
  marker.
- Enemy array base/stride (agent-reported, NOT independently verified here):
  base `DAT_0063b218`, stride `0xd68`, state at `+0xbfc`. Verify before relying on a
  whole-array sweep; the detour approach above doesn't need them.

---

## 5. The damage-RETURN lever at `FUN_0043d9e0` — NOW THE SHIPPED IMPLEMENTATION

(From the 2026-06-12 overnight session, decomp-verified; promoted from fallback
to the implementation after the game test killed the ECL-cap approach — see the
status banner. `coop.c`'s `HookedDamage` is this recipe.)

### `FUN_0043d9e0` @ `0x0043d9e0` — the player-shot damage function
```c
int __thiscall FUN_0043d9e0(int player /*ECX, has shot array*/,
                            float *target_pos, float *target_size, int *out_flag);
```
- Sweeps the player shot array (`player + 0x2444`, 0x60 slots, stride 0x364;
  active flag at slot `+0x34a`), AABB-tests each active shot against the target box,
  and **returns the total damage** dealt this frame (per-shot damage read from slot
  `+0x348`; reduced during a bomb via the `player+0x16a20` bombing flag, lines
  25887–25895). Side effects: consumes hit shots (sets slot `+0x34a = 2`, line
  25906) and spawns hit sparks (25902–25904).
- **Caller = the per-enemy damage-apply loop** in `FUN_00420620` (def 12608), line
  12822: the returned value is what gets subtracted from (accumulated into) the
  enemy's HP.

### Recipe: detour and scale only the return value
```c
int __fastcall HookedDamage(void* self, void* edx, float* pos, float* size, int* outf) {
    int r = s_orig(self, edx, pos, size, outf);   // side effects happen once, normally
    if (r > 0) { r = r / 2; if (r == 0) r = 1; }   // ~2x effective HP, never a no-op
    return r;
}
```
- Scaling the **return** (not the per-shot `+0x348` value) preserves shot consumption
  and hit-spark side effects exactly once — only the HP subtraction shrinks.
- **Floor at 1** when the original was >0, else 1-damage shots become no-ops.
- Scales ALL player-shot damage (every enemy, not just bosses) — same scope as
  Option A's all-enemies behavior, but as a damage divisor (rounding differs).

### Trade-off vs the retired ECL-cap detour (Option A, §2–§3)
Option A scales the phase cap once per set-life opcode (exact ×N, no rounding
loss, life bar drains visually correct) — but the game test proved it only ever
reaches popcorn; boss/midboss HP is initialized somewhere `FUN_00424290`'s cap
write never sees. The damage-divisor loses fractions to integer division and the
floor can overweight 1-damage shots, but it provably covers everything. If exact
boss pacing ever matters enough, the open RE task is "where is boss HP actually
set" — until then this is the lever.
