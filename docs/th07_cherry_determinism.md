# Tier 2 — Per-player cherry & the item-drop determinism coupling (verified)

The user wants **a separate cherry counter per player** (handoff §4). Cherry is also
the handoff's **#1 determinism risk**, because the live cherry value feeds a
shared-RNG item-drop decision. This note verifies the exact coupling from
`PCBdecomp.c` and states precisely what is and isn't safe.

Build-specific to th07.exe ver 1.00b (SHA256 `35467EAF…E80CA`).

---

## 1. The coupling, verified

Two enemy/bullet-death item-drop sites — `FUN_0041b3xx` @ PCBdecomp.c:10445 and
`FUN_0041b4a0` @ 10495 — make the same decision:

```c
ratio = ((DAT_0062f88c - *(int *)(DAT_00626278 + 0x88)) * 100) / DAT_0062f888;
roll  = FUN_004318d0();          // Rng_Next32() — Rng_Core x2, advances counter 0x49fe24
if (roll % 100 <= ratio) {       // cherry-gated branch
    FUN_0044e8e0(param_1, 0x2d8); // spawn the (cherry) item
    ...
}
```

Symbols:
| Symbol | Meaning |
|---|---|
| `*(int *)(DAT_00626278 + 0x88)` | **live cherry counter** (= `((int*)DAT_00626278)[0x22]`) |
| `DAT_0062f88c` | cherry numerator base (set at game/replay load, line 27895) |
| `DAT_0062f888` | cherry denominator / CherryMax (set at load, line 27896) |
| `FUN_004318d0` | `Rng_Next32` (handoff §1) — the shared RNG |

Load-time setup (PCBdecomp.c:27895–27896) ties both bounds to the same live-cherry
base: `DAT_0062f88c = replay[+8] + cherryBase; DAT_0062f888 = replay[+0xc] + cherryBase`.

---

## 2. The key refinement: the desync is SECOND-ORDER

**The `Rng_Next32()` call at line 10446/10496 is UNCONDITIONAL** — it runs whether or
not the cherry ratio would spawn an item. So:

- A per-frame cherry *difference between the two machines does NOT immediately diverge
  the RNG counter* — both machines still consume exactly one `Rng_Next32` here.
- What diverges is the **branch**: one machine spawns the item, the other doesn't.
  That changes **object state** (an item exists on one side only), which then diverges
  the RNG counter on *subsequent* frames, when that item interacts with anything that
  consumes RNG (collection → bonus spawns, etc.) or shifts object-iteration order.

So cherry-driven desync is *second-order* (via object state), not first-order (via the
roll). Practical consequence: the desync oracle (`0x49fe24`) will NOT flag the exact
frame cherry diverged — it flags some frames later. Don't expect a clean pinpoint;
expect a delayed counter split downstream of a divergent drop.

---

## 3. Design verdict for "separate cherry counter per player"

`*(int*)(DAT_00626278 + 0x88)` is the single value feeding the shared roll. Options:

- **Shared cherry pool (safe, trivial):** leave `+0x88` shared. Both machines roll on
  the same ratio → identical branches → no cherry-induced desync. Loses the
  "separate counter" feature.
- **Per-player cherry (the wanted feature — needs care):** give P2 (and P3) their own
  cherry counters (DLL-owned, like coop.c does for bombs/power via field-swap), but the
  **shared item-drop roll must read ONE agreed value** for `+0x88`. Pick a deterministic
  rule both machines compute identically, e.g.:
  - drive the roll off **P1's cherry only** (simplest; P2's counter is cosmetic/score),
    or
  - drive it off **`max(P1,P2)` or the sum** — any pure function of values both
    machines hold identically.
  As long as the value `+0x88` holds *at the instant of the roll* is byte-identical on
  both machines, determinism holds regardless of how many separate counters you display.

**Recommended:** separate *display/score* cherry counters per player, but feed the
shared item-drop roll from a single deterministic counter (P1's, or the team sum
maintained identically on both ends). This delivers the user's feature without
breaking lockstep. Validate with the `0x49fe24` counter harness on a border-heavy
stage (cherry-dense) — see the harness procedure in handoff §4b.

---

## 4. Field-swap mechanics (mirror coop.c's resource handling)

coop.c already field-swaps P2's bombs/power into the shared res struct around P2's
update (`RES_BOMBS 0x68`, `RES_POWER 0x7c`). A per-player cherry counter would follow
the same pattern at `+0x88` — BUT note the asymmetry: bombs/power are read only during
the owner's update, whereas `+0x88` is read by the **enemy item-drop code** (outside
any player update), so a naive field-swap won't cover the roll site. That is exactly
why the roll must be pointed at a single agreed value (§3) rather than whichever
player's counter happens to be swapped in. Implement the per-player counters as
DLL-owned state and write the agreed value into `+0x88` for the duration of enemy
updates, or detour the two drop sites (`FUN_0041b3xx`:10446 / `FUN_0041b4a0`:10496) to
read the agreed value directly.

---

## 5. Offsets recap (for the Tier-2 implementer)
- Live cherry counter: `*(int*)(DAT_00626278 + 0x88)`  (res-struct index `[0x22]`)
- Cherry roll bounds: `DAT_0062f88c` (num), `DAT_0062f888` (den)
- Shared RNG used: `FUN_004318d0` (Rng_Next32), counter oracle `0x0049fe24`
- Drop sites to detour (if reading agreed cherry directly): PCBdecomp.c:10446, 10496
- (handoff §4 also lists the score sub-struct cherry/point/graze display fields off the
  game-manager object: `+0x20a28` cherry, `+0x20a24` points, `+0x20a2c` graze, border
  state `+0x209f0` — those are display/score, NOT the roll input above.)

---

## 6. Decision + implementation plan (2026-06-11)

**User decision (2026-06-11): "separate counts per player, SHARED border."** Each
player gets their own cherry count for score/display; the border (bullet-cancel)
activates off the TEAM total; the shared item-drop roll is fed by the team value.
(The other options — P1-drives-all, fully-per-player-borders, keep-shared — were
declined.)

### The determinism re-framing that makes this SAFE by construction
Delay-based lockstep means **both machines run the entire game sim on the same
merged input word every frame**, so every value that is a pure function of
inputs+seed is *already* byte-identical on both ends — including the live cherry
counter `+0x88`, item positions, collisions, and the border state. The §2
"second-order desync" warning bites **only if a per-player counter is fed into
the roll** (a value the two machines might compute differently). The user's choice
keeps the gameplay-affecting cherry SHARED, so:

> **Leave `+0x88` (the live cherry that drives the roll AND the border) exactly as
> the engine maintains it — the shared team value. Do NOT swap per-player values
> into it.** Then the roll site (10445/10495) and border read need NO change and
> stay deterministic for free. "Separate counts" becomes a **display/score
> attribution layer** that never feeds the sim.

### What "separate counts" actually requires
A per-player count = "how much cherry did THIS player collect." That needs to
attribute each collected cherry item to P1 vs P2 — i.e. P2 must participate in
**item collection**, and the credit must be split. Findings:

- **`FUN_0043e4e0` (PCBdecomp.c:26056) is NOT the item-collect routine** — it is the
  player-area **overlap test**: `__thiscall(player, float *pos, float *size)` →
  returns 1 if the box overlaps the player's collect box (`+0x978/+0x97c/+0x984/
  +0x988`) and player state ∈ {0,3,4}. It is param-relative (like the bullet/laser
  primitives coop.c already detours). **coop.c's comment mislabels it "item
  collect" — corrected there.**
- The **collection EFFECT** (credit power/points/cherry, spawn bonuses) lives in
  the **caller** (the item-update loop around PCBdecomp.c:20435), not the
  primitive. So crediting cherry per-player means detouring/raeding that *caller*,
  not the overlap test.
- ⚠️ **The item-collection loop is decompiled ambiguously** (parameter-type
  confusion — e.g. `FUN_0043e4e0(local_28 + 0x24c, &local_20)` passes an item
  field where the player base is expected). Reversing the exact cherry-credit
  store reliably will likely need the **disassembly** of the collect loop, not just
  `PCBdecomp.c`. The live-cherry increment was not pinned to a single `+=` site;
  cherry is managed via the bounds (`DAT_0062f88c/888/890`) and the live counter
  `[0x22]`, with resource accessors `FUN_004325c0/5e0` in the collect path.

### Implementation plan (determinism-safe, staged)
1. **P2 item collection** (prerequisite, also a standalone co-op win): make items
   collectable near P2. Cleanest seam = detour the overlap test `FUN_0043e4e0` and
   re-invoke with ECX=P2 (mirror the bullet/laser detours); the caller then
   collects items near either player into the shared pool. Determinism-safe: P2's
   position is the synced merged input, so both machines collect identically.
2. **Per-player attribution (display only):** to split the count, detour the
   collect caller (needs the disasm per the caveat) so a cherry collected via the
   P2-overlap branch increments a DLL-owned `s_p2Cherry` while the P1-overlap branch
   feeds `s_p1Cherry`; the engine's shared `+0x88` keeps rising as today.
3. **HUD:** draw the two counts (deferred until a P2 HUD exists — today P2 resources
   are logged to `coop_log.txt`).
4. **Border:** no change — it already reads the shared `+0x88` = team total, which
   is exactly "shared border" per the decision.

Net: the gameplay/determinism surface is untouched (shared cherry); the new code is
a display attribution layer gated on P2 item collection. Validate later with the
`0x49fe24` oracle on a border-heavy stage (per the deferred netplay test).
