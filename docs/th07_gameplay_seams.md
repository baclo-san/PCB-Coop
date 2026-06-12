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

## 2b. Resources, anti-tamper & a hidden determinism interaction (verified)

The resource struct (`*(int*)0x00626278`, reached as `*(scoreSingleton 0x626270 + 8)`)
field offsets `coop.c` uses are **confirmed against the accessors**:

| Field | Offset | Evidence |
|---|---|---|
| Lives (float) | `+0x5c` | `FUN_0042d5cd` line 17723: `*(float*)(*(p+8)+0x5c) += delta` |
| Bombs (float) | `+0x68` | `FUN_0042d612` line 17745: `*(float*)(*(p+8)+0x68) += delta` |
| Checksum      | `+0xb0` | `FUN_004012b0` line 2874: `*(*(p+8)+0xb0) = FUN_0042d7be()` |

**Anti-tamper** (matches `coop.c`'s comments): each accessor first calls
`FUN_00404fe0`; if it reports tampering it fills `DAT_00575950` with `0xffffffff`
(0xb4 dwords) → crash. `FUN_004012b0` is the **heal**: it recomputes the checksum
(`+0xb0`), two random canaries (`+0x3c`, `+0x98`), a copy (`+0xac`), and a derived
guard float at `player+0x9614`. So `coop.c` calling `FUN_004012b0` after restoring
P1's values correctly re-establishes the whole invariant. **Note `FUN_004012b0` is
the checksum recompute, NOT the visible HUD draw** — the lives/bomb *sprite* draw is
a separate, not-yet-located function (needed for a real P2 HUD).

> ⚠️ **Determinism interaction for the netcode integration:** `FUN_004012b0`
> **consumes 2 shared-RNG calls** (`Rng_Next32` at lines 2868/2870 for the canaries).
> `coop.c` invokes the heal whenever P2's update changed the checksum (i.e. on frames
> P2 gains/loses a life or bomb). That advances the shared RNG counter
> (`0x0049fe24`). In 2-player netplay this is harmless **only because both machines
> run P2's identical simulation and so consume the same calls in lockstep** — but it
> means the co-op RNG stream diverges from a vanilla/single-player one. Anything that
> makes P2's resource events differ between machines (e.g. a future *per-player* lives
> model that isn't byte-identical) will desync here. Validate with the `0x0049fe24`
> counter oracle after wiring the netcode in.

## 2c. Border / cherry display & the per-player-cherry seam (user wants this)

The user wants **separate cherry counters per player**; the likely-safe design is
"separate display counter, **shared** border activation" (per-player borders that each
cancel bullets would trivialize the game). The score sub-struct lives at
`gameManager+8` (`= *(scoreSingleton 0x626270 + 8)` = the `0x626278` struct); the
display fields (handoff §4) are **confirmed**:

| Field | Meaning | Decomp |
|---|---|---|
| `+0x209f0` | **border state**: 1=FullPower, 2=SupernaturalBorder, 3=CherryPointMax, 4=BorderBonus; 0=inactive | set 15444, read 15633/16760 |
| `+0x209ec` | border bonus value | 15449 |
| `+0x209fc` | **border banner anim counter** 0→0xb3 (border ends when `>0xb3`) | 16761/16769/16770 |
| `+0x209e0` | banner float Y | 15439 |
| `+0x20a28` | cherry display (×10); `= cherryMax − liveCherry` at tally | 15567, 16392 |
| `+0x20a24` | point items (×50000) | 15559, 16391 |
| `+0x20a2c` | graze (×500) | 15563, 16393 |
| `+0x209b8` | total score | 15620, 16812 |

Key functions:
- **`FUN_00427c81(this, bonus, state)` @ `0x00427c81`** — **border activation** (sets
  `+0x209f0=state`, `+0x209ec=bonus`, resets the banner). The "shared border
  activation" seam: let either player's fill trigger it, once.
- `FUN_0042adab(this)` @ `0x0042adab` — border-banner **tick** (advances `+0x209fc`,
  ends the border at `>0xb3`).
- `FUN_00429c42(this)` @ `0x00429c42` — **end-of-stage tally** (writes the display
  cherry/point/graze from the shared totals).

**Per-player cherry plan:** keep the determinism-critical pool (`DAT_0062f888/88c` +
`(0x626278)+0x88`, which feeds the item-drop roll — see §2) **shared**, so the
RNG-coupled drop stays byte-identical on both machines. Track each player's *display*
cherry contribution in DLL-owned state (incremented when that player collects), and
drive border activation off the shared pool (or "either player full"). This avoids
adding determinism-feeding state. Confirm with the `0x0049fe24` counter oracle.

## 2d. Player death FSM & the resurrection seam (Tier 3 — revive-a-partner)

The player update `FUN_00441fb0` @ `0x00441fb0` (the fn `coop.c` piggybacks for P2)
dispatches on the **state byte `+0x2408`** (= coop.c `OFF_STATE`):
`0`=play, `1`=respawn-entering, `2`=dying, `3`=respawn-invuln, `4`=border. State `2`
routes to the **death/respawn handler `FUN_00440cf0` @ `0x00440cf0`**, which is where
a "revive your partner" mechanic injects. Reading it (lines 26731+) pins these:

| Field / call | Meaning | Line |
|---|---|---|
| `+0x16a08` | **deathbomb counter** — frames since the fatal hit; death COMMITS once `0x1d (29) <` it | 26748 |
| `+0x23f8` | **respawn timer** — set on death, decremented each frame; hits 0 → respawn (restore control, drop power items) | 26778-26779 |
| `FUN_0042d5cd(-1)` | **lose a life** on death commit | 26763 |
| `FUN_0048b8a0()` | lives-remaining query; `>0` → respawn (`return 1`), `==0` → set game-over | 26761-26770 |
| `DAT_0062f64d = 1` | **team game-over flag** (= coop.c `ADDR_GAMEOVER`) when out of lives | 26770 |
| `DAT_00626278 + 0x7c = 0` | **power reset** to 0 on respawn (confirms coop.c `RES_POWER` 0x7c) | 26787 / 26799 |
| `+0x930 / +0x934` | player X / Y, reset to center on respawn (confirms coop.c `OFF_POS_X/Y`) | 26750-26751 |

**Resurrection design (mirrors th06 mod's "hold focus + release shot near dead
partner for ~90 frames → spend a life to revive"):** while the partner is in state
`2` and inside its deathbomb window (`+0x16a08 <= 29`), if the reviver holds the
revive input for the required frames, force the partner back to state `0` and clear
`+0x16a08`/`+0x23f8`, **and spend the REVIVER's life** (`FUN_0042d5cd(-1)` on the
reviver) instead of letting the partner's death commit (which would call the
life-loss + possibly set `0x0062f64d`). `coop.c` already intercepts the game-over
flag for P2 (ghost mode); resurrection replaces ghost mode by catching the partner
*before* the commit. This is Tier-3 and needs live testing — the exact revive input,
frame count, and whether revive works mid-respawn (state 1/3) are tuning calls.

> This section also independently re-confirms coop.c's `OFF_STATE` (0x2408),
> `RES_POWER` (0x7c), `OFF_POS_X/Y` (0x930/0x934) and `ADDR_GAMEOVER` (0x62f64d).

## 3. Quick map of related functions (leads, not fully reversed)
| Addr | Role | Evidence |
|---|---|---|
| `FUN_0043d9e0` | player-shot → target damage (return = dmg) | def 25836, callers 12822/12824 |
| `FUN_0044e8e0` | spawn/emit (items, effects) — consumes RNG | called 10448, 3179, 3609 |
| `FUN_004318d0` | `Rng_Next32` | the `%100` roll at 10446 |
| `FUN_00431900` | `Rng_NextFloat` | item angle/vel at 10441/10443 |
| player shot array | `player+0x2444`, 0x60 slots × 0x364, active@+0x34a, dmg@+0x348 | 25866–25916 (matches coop.c OFF_SHOTS/SHOT_STRIDE/SHOT_COUNT) |
| `FUN_00441fb0` | player update (state dispatch on `+0x2408`); coop.c piggybacks it | def 27207 |
| `FUN_00440cf0` | dying/respawn handler (death commit, life loss, respawn) | def 26731 |
| `FUN_00420620` | per-enemy update + damage-apply (the only caller of `FUN_0043d9e0`) | def 12608, registered 13457 |

> These three docs together: `th07_netplay_handoff.md` (original, broad),
> `th07_integration_forkA.md` (network integration + Fork A), this file (gameplay).
