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
