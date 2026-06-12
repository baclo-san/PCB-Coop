# TH07 Co-op — Gameplay seams (boss HP, cherry determinism)

**Status date:** 2026-06-12 (unattended session). Decomp-verified seams for the
**gameplay-side** co-op features (Tiers 1–2 in `th07_netplay_handoff.md` §4).
Line numbers reference `PCBdecomp.c` (ver 1.00b dump). Addresses are VAs and are
**build-specific** — re-pin if the binary changes.

---

## 1. Boss / enemy HP scaling (Tier 1 — the "double DPS" balance lever)

Two players ≈ 2× firepower, so bosses/enemies die ~twice as fast and timing/spell
pacing breaks. The cleanest single lever found:

### `FUN_0043d9e0` @ `0x0043d9e0` — the player-shot damage function
```c
int __thiscall FUN_0043d9e0(int player /*ECX, has shot array*/,
                            float *target_pos, float *target_size, int *out_flag);
```
- Sweeps the player shot array (`player + 0x2444`, **0x60 slots**, stride **0x364**;
  active flag at slot `+0x34a`), AABB-tests each active shot against the target box,
  and **returns the total damage** dealt this frame (per-shot damage read from slot
  `+0x348`; halved-ish during a bomb via the `player + 0x16a20` bombing flag, lines
  25887–25895). It also has **side effects**: it consumes hit shots (sets slot
  `+0x34a = 2`, line 25906) and spawns hit sparks (25902–25904).
- **Caller = the enemy/boss damage-apply loop** (line 12822):
  ```c
  local_1c = FUN_0043d9e0(local_40 + 0x2b0c, local_40 + 0x2b3c, &local_18);
  ...
  if (0 < local_1c) { /* apply local_1c to the target's HP, hit FX, etc. */ }
  ```
  `local_40 + 0x2b0c` is the player (shot holder); `+0x2b3c`/`+0x2b48` are the
  target's hit boxes. The returned `local_1c` is what gets subtracted from HP.

### Recommended implementation (NOT yet shipped — needs a live test)
Detour `FUN_0043d9e0` and **scale only the return value**:
```c
static CoopDmgFn s_orig;
int __fastcall HookedDamage(void* self, void* edx, float* pos, float* size, int* outf) {
    int r = s_orig(self, edx, pos, size, outf);   // side effects happen once, normally
    if (r > 0) { r = r / 2; if (r == 0) r = 1; }   // ~2x effective HP, never a no-op
    return r;
}
```
- Scaling the **return** (not the per-shot `+0x348` value) preserves shot consumption
  and hit-spark side effects exactly once — only the HP subtraction shrinks.
- **Floor at 1** when the original was >0, else `1`-damage shots become no-ops and weak
  shot types can never kill anything.
- Make it a toggle (ini / F-key) and **default OFF** until balance is tested with two
  live players — scaling factor (×1.5? ×2?) is a tuning/design call for the user.
- ⚠️ This scales ALL player-shot damage (every enemy, not just bosses). If only bosses
  should scale, gate on the target: bosses are the `local_40`-rooted path; fairies use
  other call sites — confirm by which caller is hit (set a breakpoint / log the caller
  return address) before narrowing.

### Alternative levers (if return-scaling misbehaves)
- Scale the per-shot damage field `+0x348` at shot-creation time (player-side) — but
  that also changes the bomb-damage `/3` path and any display.
- Double enemy HP at the enemy-init / ECL `set-life` opcode — but PCB enemy HP comes
  from **ECL bytecode in `th07.dat`**, not the exe, so you'd detour the ECL opcode
  handler (the big interpreter switch) — more invasive than return-scaling.

---

## 2. Cherry system & the #1 determinism risk (Tier 2 — per-player cherry)

PCB's enemy item-drop roll is **gated by cherry fill**, and the roll consumes the
**shared** RNG — so if two machines hold different cherry, the drop branch diverges
and they desync. Re-verified exactly where the handoff said (lines 10445/10495):

```c
// FUN around 0x... item-drop, line 10441-10447:
fVar8 = Rng_NextFloat();  // 10441  item angle      } consumed UNCONDITIONALLY,
param_1[2] = fVar8*2pi - pi;                         } identical on both machines
fVar8 = Rng_NextFloat();  // 10443  item ang-vel    } (cherry-independent)
*param_1 = fVar8*0.0314 - 0.0157;
uVar6 = ((DAT_0062f88c - *(int*)(DAT_00626278 + 0x88)) * 100) / DAT_0062f888;  // 10445 cherryRatio
uVar7 = Rng_Next32();     // 10446  the roll
if (uVar7 % 100 <= uVar6) {            // 10447  ← CHERRY-SENSITIVE BRANCH
    FUN_0044e8e0(param_1, 0x2d8);       //        spawn the item (consumes more RNG inside)
    ...
}
```
**Determinism nuance:** the two `Rng_NextFloat()` (10441/10443) and the `Rng_Next32()`
roll (10446) are consumed **regardless** of cherry — those don't desync. The *branch*
at 10447 (`roll % 100 <= cherryRatio`) is the sensitive point: if it goes different
ways, one machine calls `FUN_0044e8e0` (which consumes additional RNG) and the other
doesn't → **counter divergence from that frame on**. The harness counter `0x0049fe24`
catches this precisely.

### Cherry globals (confirmed)
| Symbol | Meaning | Decomp |
|---|---|---|
| `DAT_0062f888` | Cherry **max** (denominator) | 10445, 4222/4253 |
| `DAT_0062f88c` | Cherry **target/current ceiling** | 10445, 4087 (`= DAT_00626278[0x22]`), 12491 (decremented on use) |
| `*(int*)(DAT_00626278 + 0x88)` | **live cherry** consumed | 10445 |
| `gameManager+8 → +0x20a28` | Cherry (cumulative ×10) | 16392 (end-of-stage tally) |

### Design implication for per-player cherry (user's open decision)
- **Shared cherry pool** → automatically determinism-safe (one value feeds the roll).
- **Per-player cherry** → the cherry numbers that feed line 10445 become duplicated
  state that MUST be byte-identical on both machines, AND you must pick *which*
  player's ratio drives the (shared) roll. Validate any choice with the counter
  oracle. The handoff's suggested sweet spot — "separate counter, shared border
  activation" — keeps the determinism-feeding number shared while displaying
  per-player counts. Decide when the mechanic is visible/playable (needs the user).

---

## 3. Quick map of related functions (leads, not fully reversed)
| Addr | Role | Evidence |
|---|---|---|
| `FUN_0043d9e0` | player-shot → target damage (return = dmg) | def 25836, callers 12822/12824 |
| `FUN_0044e8e0` | spawn/emit (items, effects) — consumes RNG | called 10448, 3179, 3609 |
| `FUN_004318d0` | `Rng_Next32` | the `%100` roll at 10446 |
| `FUN_00431900` | `Rng_NextFloat` | item angle/vel at 10441/10443 |
| player shot array | `player+0x2444`, 0x60 slots × 0x364, active@+0x34a, dmg@+0x348 | 25866–25916 (matches coop.c OFF_SHOTS/SHOT_STRIDE/SHOT_COUNT) |

> These three docs together: `th07_netplay_handoff.md` (original, broad),
> `th07_integration_forkA.md` (network integration + Fork A), this file (gameplay).
