# Tier 2 — Per-player cherry & the item-drop determinism coupling (verified)

The user wants **a separate cherry counter per player** (handoff §4). Cherry is also
the handoff's **#1 determinism risk**, because the live cherry value feeds a
shared-RNG item-drop decision. This note verifies the exact coupling from
`PCBdecomp.c` and states precisely what is and isn't safe.

Build-specific to th07.exe ver 1.00b (SHA256 `35467EAF…E80CA`).

> **§7 (bottom) tracks the cherry-GAIN investigation** — where Cherry/Cherry+
> actually rise (shots, graze, items). Partially traced; some wiki-level claims
> not yet confirmed in the binary.

---

## 0. The three cherry values (PCB) — verified mapping

PCB tracks **three** cherry quantities, all stored as deltas from a shared base
`cherry_base = *(int*)(DAT_00626278 + 0x88)` (res index `[0x22]`). Each on-screen
value = `<global> − cherry_base`; the HUD draws all three this way (PCBdecomp.c:
4216 / 4253 / 4284):

| Value | Global | On-screen | Drives |
|---|---|---|---|
| **Cherry** | `DAT_0062f88c` | `DAT_0062f88c − base` | **point-item value** (collect cases 1 & 7) and the **item-drop roll** (10445/10495). The "usual" cherry — gameplay-secondary. |
| **Cherry Max** | `DAT_0062f888` | `DAT_0062f888 − base` | the cap / roll denominator |
| **Cherry+** | `DAT_0062f890` | `DAT_0062f890 − base` | **the cherry BORDER** (supernatural border / bullet-cancel). The critical one. |

- `cherry_base` (`+0x88`) is a *reference*, written once (a reset @18124) — NOT a
  running per-cherry counter. (Earlier notes here that called `+0x88` "the live
  cherry counter" were imprecise; the meaningful quantities are the three deltas.)
- **Cherry+ is recomputed every frame while the border is active** (player STATE 4),
  from the player's own border timer:
  `DAT_0062f890 = (player+0x16a08 * 50000) / (player+0x16a14) + cherry_base`
  (PCBdecomp.c:26914, in the state processor `FUN_00441330`). `+0x16a08` = border
  countdown, `+0x16a14` = border duration — both **per-player**. When the border
  ends (`+0x16a08 < 1`), `FUN_00441670` banks the bonus (score += `Cherry·10`) and
  collapses Cherry+ back to base (26971–26974).
- **Item collection adds to NO cherry value** — it only *reads* Cherry to scale
  point items. See `docs/th07_item_collect_credit.md`.

### ⚠️ Co-op implication for the SHARED-border decision (border ≠ bombing — CORRECTED)
**The cherry border and the spell-card bomb are TWO SEPARATE systems** (user, verified
in-binary 2026-06-11). An earlier note here wrongly claimed "bombing IS the border" —
it is not:

- **Cherry border = AUTOMATIC, FREE.** When the shared Cherry+ value (`+0x9620` on the
  score struct) reaches `cherry_base + 50000`, `FUN_0042f5a2` clamps it and calls the
  border-start `FUN_00441960` directly (`PCBdecomp.c:18655`; also the load-with-full-gauge
  case at `:27439`). No bomb bit, no bomb stock — just the gauge hitting max. Border-start
  sets state→4 and the per-player border timer `+0x16a08`=0x21c / duration `+0x16a14`.
- **Spell-card bomb = separate.** Pressing bomb runs the `+0x16a20` "bombing" path in
  `FUN_004409f0` (`:26676`); it never starts a border. **No bombs are ever spent by the
  cherry border.**
- **Pressing bomb DURING the border pops it early** (vanilla): in state 4, `FUN_004409f0`
  takes the break branch → `FUN_00441bd0` (`:26720`) — no spell card, no bomb spent, just
  ends the border with no bonus.
- **Getting hit DURING the border pops it early** (vanilla): the collision primitives
  `FUN_0043e260`/`FUN_0043e6b0` see state 4 and call `FUN_00441bd0` param-relative
  (`:26003`/`:26125`) instead of the death path — break, not death.
- **Timeout ends + banks:** `FUN_00441330` state-4 calls `FUN_00441670` when `+0x16a08`<1
  → banks `(Cherry)·10` to score and resets `DAT_0062f890` to base (`:26971`/`:26974`).
  `FUN_00441330` also recomputes the single global gauge `DAT_0062f890` from the
  *per-player* timer each frame during state 4 — so two players each in state 4 must run
  identical timers or the gauge fights.

#### Two constraints discovered when the naive "two real borders" version was play-tested
1. **The border ring is a SINGLE fixed effect slot.** `FUN_00441960` spawns the ring via
   `FUN_0041c610(0x1c, pos, 4, …)`, and `FUN_0041c610` computes the slot as
   `base + 0x1c + (4+400)*0x2d8` (`PCBdecomp.c:10882`) — **index 404 is hardcoded for the
   border ring**. So running `FUN_00441960` a second time for P2 re-grabs slot 404 and
   *steals P1's ring*: only one ring can exist, and whichever player updates last (P2)
   owns its position → "ring only on P2" bug.
2. **`FUN_00441bd0` is `__thiscall(player, int flag)` with `ret 4`** (prologue
   `mov [ebp-0x24],ecx`; epilogue `ret 0x0004`, verified in the binary). The `(0)`/`(1)`
   at its call sites are the real stack flag, not Ghidra noise. Calling it as ECX-only
   over-pops the stack by 4 → crash (was the "P2 hit crashes" symptom). `FUN_00441960`
   and `FUN_00441670` are plain `ret` (ECX-only), so those are fine.

> **DECIDED (user): option A — ONE synced team border. IMPLEMENTED in coop.c (F4),
> builds clean; needs in-game play test.** Because of constraint (1), P2 does NOT get its
> own ZUN border. **P1 runs the one real, fully-vanilla border** (ring + gauge + bonus,
> auto-triggered, no bomb involvement). **P2 rides along in a ringless "shadow" border:**
> `UpdateTeamBorder` (polled each frame, just before P2's update) sets P2's state to 4 +
> the border-active flag `0x240d`=1 + copies P1's six border-timer dwords (`0x16a00..14`),
> but leaves P2's ring ptr `0xb7e6c`=0 so P1 keeps the single ring (it follows P1). P2 is
> then genuinely in state 4, so it is invincible AND ZUN's own collision
> (`FUN_0043e260/6b0`) + bomb handler (`FUN_004409f0`) pop the border on a hit / bomb
> exactly like P1 (no death, no spell, no bomb spent). We hook ONLY the break leaf
> `FUN_00441bd0` (correct `__thiscall(player,int flag)` signature, flag forwarded) to
> propagate a pop by either player to the other → one team border ends for both.
> - **Timeout:** P1 ends in its own update first; the poll then retires P2's shadow
>   (state→3, no bank) before P2's update reaches the bank — so it banks once. (User:
>   double-bank would also be acceptable as long as it doesn't break sync.)
> - **P2's bomb is NOT masked** — outside the border it casts a normal spell card (spends
>   P2's own bomb); during the border `0x240d`=1 routes it to the pop path instead.
> - **P2's collision runs while bordered** (`P2CollisionSkipped` only skips P2 when P1 is
>   bordered and P2 is *not*, e.g. respawning), so a hit on P2 pops the team border.
>
> Superseded attempts: (i) P2's bomb *starts* a border and *spends a bomb* — wrong, the
> cherry border is automatic and free; (ii) mirror `FUN_00441960` onto P2 for a second
> real border — broke on the single ring slot (1) and the wrong `FUN_00441bd0` signature
> (2). Known cosmetic gap: only P1 shows the ring (engine has one). A second visible ring
> would need spawning a type-0x1c effect in a free player-effect slot (5–7) via the
> effect-manager pointer — more RE; deferred.
>
> Rejected earlier: (B) independent borders + gauge-follows-P1; (C) full per-player
> borders + 2nd HUD (user: trivializing).

Determinism is unaffected in all cases (cherry globals are lockstep sim state).

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

Symbols (see §0 for the full three-value model):
| Symbol | Meaning |
|---|---|
| `*(int *)(DAT_00626278 + 0x88)` | `cherry_base` — the shared reference all three values subtract from (= `((int*)DAT_00626278)[0x22]`) |
| `DAT_0062f88c` | **Cherry** raw (on-screen Cherry = `DAT_0062f88c − base`); drives this roll + point-item value |
| `DAT_0062f888` | **Cherry Max** raw (= cap / roll denominator) |
| `FUN_004318d0` | `Rng_Next32` (handoff §1) — the shared RNG |

So `ratio = (Cherry · 100) / CherryMax` with `Cherry = DAT_0062f88c − base`,
`CherryMax = DAT_0062f888 − base`. Load-time setup (PCBdecomp.c:27895–27896) ties all
three raws to the same base: `DAT_0062f88c = replay[+8] + cherry_base`,
`DAT_0062f888 = replay[+0xc] + cherry_base`, `DAT_0062f890 = replay[+0x10] + cherry_base`.

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

### Score-struct border/display functions (for the P2 cherry HUD work)
(From the 2026-06-12 overnight session, decomp-verified. These are the SCORE-side
border state machine — the banner/HUD layer — distinct from the player-side border
`FUN_00441960`/`FUN_00441bd0`/`FUN_00441670` in §0.) Score sub-struct =
`gameManager+8` (the `0x626278` struct); display-field line cites: border state
`+0x209f0` set 15444 / read 15633, 16760; bonus `+0x209ec` 15449; banner anim
counter `+0x209fc` 0→0xb3 (16761/16769/16770); banner float Y `+0x209e0` 15439;
cherry display `+0x20a28` 15567/16392; points `+0x20a24` 15559; graze `+0x20a2c`
15563; total score `+0x209b8` 15620/16812.

| Fn | Role |
|---|---|
| `FUN_00427c81(this, bonus, state)` @ `0x00427c81` | **border banner activation** — sets `+0x209f0=state`, `+0x209ec=bonus`, resets the banner anim |
| `FUN_0042adab(this)` @ `0x0042adab` | border-banner **tick** — advances `+0x209fc`, ends the banner at `>0xb3` |
| `FUN_00429c42(this)` @ `0x00429c42` | **end-of-stage tally** — writes display cherry/point/graze from the shared totals |

---

## 6. Decision + implementation plan (2026-06-11)

> **⚠️ DECISION REVISED (user, 2026-06-12): per-player cherry DISPLAY is DROPPED.**
> With the shared team border settled (§0), a single shared cherry counter is the
> design — two counters would be confusing. Everything below about per-player
> display attribution is RETIRED (kept for the record); the cherry-gain trace
> (§7) is no longer needed for any planned feature. **The P2 HUD (lives/bombs/
> power) is still wanted** — that's an item-credit/HUD task, not a cherry task.

**User decision (2026-06-11, superseded above): "separate counts per player,
SHARED border."** Each player gets their own cherry count for score/display; the
border (bullet-cancel) activates off the TEAM total; the shared item-drop roll is
fed by the team value. (The other options — P1-drives-all,
fully-per-player-borders, keep-shared — were declined.)

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
4. **Border (CORRECTED — see §0):** the border is driven by **Cherry+**
   (`DAT_0062f890`), which `FUN_00441330` recomputes from the *per-player* border
   timer during state 4 — so P2's piggyback update clobbers it. For the chosen
   **shared border**, coop.c must reconcile `DAT_0062f890` around P2's update
   (snapshot P1's value before, restore after — so the one border follows P1). This
   is a gameplay-correctness fix; determinism is unaffected (all cherry globals are
   lockstep sim state).

Net: the gameplay/determinism surface is untouched (shared cherry); the new code is
a display attribution layer gated on P2 item collection. Validate later with the
`0x49fe24` oracle on a border-heavy stage (per the deferred netplay test).

### Update (2026-06-11): cherry fill ≠ item collection — see the credit map
Tracing the item-collect loop (`FUN_00432990`) showed it only **reads** cherry
(`+0x88`) to scale point values; it never adds to it. The live cherry `+0x88` is
written just once (a reset), and cherry-*gain* is driven by the bound
`DAT_0062f88c` **decrementing** (PCBdecomp.c:12491/26707/26821), likely off the
shot/graze path — **not yet traced**. So per-player CHERRY is a separate attribution
problem from per-player POWER/BOMBS (which DO come from the collect loop). Full
verified map + the power/bombs attribution design (collector-flag + field-swap+heal,
determinism-safe) is in **`docs/th07_item_collect_credit.md`**. Implement separate
power/bombs from there now; separate cherry display needs the `DAT_0062f88c`-gain
trace first.

---

## 7. Cherry-GAIN investigation (2026-06-11 — RETIRED 2026-06-12)

> **No longer needed:** this trace only served the per-player cherry *display*,
> which the user dropped (§6 banner). Kept because the findings (graze ≠ cherry,
> the accumulator structure, the pinned shot-hit path) are real RE that may help
> later. Do not resume unless a feature needs it.

User's initial (wiki-level) claims: cherry rises from (a) shots landing on enemies
(less when focused; also feeds Cherry+), (b) certain item pickups (cherry items /
petals / bullet-deletion stars), (c) grazing. **Update: (c) grazing was re-checked
in-game and is FALSE** (see below). (a) and (b) still need tracing.

### Structural finding that complicates this
The three cherry raws (`DAT_0062f88c/888/890`) are, in every site found, only
**reset-to-base or decremented** — never simply `+=`'d during play. So cherry "gain"
is **not** a direct increment of those globals; it is indirect (a running counter /
accumulator the cherry values derive from, or `cherry_base` moving). That running
counter is not yet isolated — the crux still open.

### (c) Graze — ✅ RESOLVED: grazing does NOT give cherry (wiki was wrong)
**User re-checked in-game (2026-06-11): grazing does not raise cherry at all.** This
matches the RE exactly:
- Per-bullet player interaction `FUN_00420490` calls the **graze range test
  `FUN_0043e3b0`** (graze box = player `+0x960/+0x964/+0x96c/+0x970` ± 20px),
  throttled per bullet (`+0x2bcc % 6 == 0` plus a flag, `+0x2e29` bit 5).
- On a graze, `FUN_0043e3b0` calls credit fn **`FUN_0043eb90`**, which increments the
  graze counters `res+0x14` (cap 9999) & `res+0x18` (cap 999999), adds **200** to
  score (`res+0x04`), and bumps `DAT_012fe0d0 += 0x9c4 + ((Cherry)/0x5dc)*0x14`.
- It touches **no cherry value**. So `DAT_012fe0d0` is a **score/graze-bonus**
  accumulator (cherry-scaled), NOT a cherry counter — drop it as a cherry lead. The
  graze→cherry claim is dead.

### (a) Shots landing & (b) item pickups — NOT yet traced
- Shot→enemy damage feeds the enemy accumulator `+0xd18` (boss-HP doc); whether it
  also feeds cherry (and the focus reduction) is unverified — find the shot-hit
  handler, look for a cherry / `DAT_012fe0d0` write.
- **Lead pinned (2026-06-12 overnight): the shot-hit path is now located** —
  damage calc = `FUN_0043d9e0` (boss-HP doc §5), applied by the per-enemy update
  `FUN_00420620` at PCBdecomp.c:12822. The cherry-gain write, if shots are the
  source, should be near that apply site (or inside `FUN_0043d9e0`'s per-hit loop,
  which also knows the focus state via the player). Start the (a) trace there.
- Cherry/petal/star items: collect cases 1 & 7 *read* Cherry but no cherry *add* was
  seen there; the bullet-deletion-star (spell-capture clear) path is untraced.

### Next step for 3b
Graze is ruled out and `DAT_012fe0d0` is a score-bonus (not cherry). So trace the
**shot→enemy hit handler** (the most likely real source — the player said shots
landing raise cherry, less when focused) to the single running cherry counter, then
confirm which of Cherry (`DAT_0062f88c`) / Cherry+ (`DAT_0062f890`) it feeds and the
focus dependency; then the bullet-deletion-star / cherry-item paths. Only then is a
faithful per-player cherry *display* possible. None of this blocks determinism —
cherry stays shared and lockstep-identical regardless.
