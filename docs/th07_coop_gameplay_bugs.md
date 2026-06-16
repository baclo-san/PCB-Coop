# TH07 (PCB) Co-op — Gameplay Bug Backlog

Source: user + friend played many **local 2-player** runs over Parsec (one machine,
shared screen — so these are NOT netplay/desync issues; they reproduce in plain local
co-op). Logged 2026-06-15. These are **independent of the §8l guest→host transport
desync** and are intended for an unattended (nightshift) session — most are
well-specified enough to fix + self-verify without the user. **Crashes first.**

> Reproduce in local co-op (no netplay needed): inject `th07_coop.dll`, F9 to spawn P2,
> play with the P2 controls (IJKL move / Space shot / U focus / O bomb). Note the
> last-good commit before each change; build with `build.ps1`; keep the DLL 32-bit.

## P0 — crashes (highest priority; a crash on one machine also = "peer lost" in netplay)

- **C1 — P1 Sakuya (either shot) + P2 Reimu (either shot) crashes when Reimu's bullets
  hit an enemy.** Other char combinations seem fine. (Notes #10.) Strong lead: the
  per-player shot/aim graft — cross-char P2 shot interacting with the hit handler.
  Related RE already pinned: `BuildP2TargetBlock` / SakuyaA aim cone (handoff §8c),
  shot-hit path `FUN_0043d9e0` / `FUN_00420620`. Reproduce with that exact combo, then
  bisect: does it need P2=Reimu specifically, or any P2 whose bullets reach the hit
  path while P1=Sakuya? Instrument the hit handler with the player/shot identity.
- **C2 — "Death fairy" at end of stage 4 (high-HP fairy with an apparent on-hit
  invuln timer, takes shots but no damage) crashes the game mid-firing**, right before
  the boss cutscene. Reason unknown. (Notes #11.) **The relevant `coop_log.txt` is in
  `build/` from that session** — read it first. Likely the same shot/hit or enemy-state
  interaction family as C1; check whether it also needs a specific char combo.

## P1 — resource/extend sharing (mechanics, determinism-safe, clear specs)

- **B1 — Point-item extend is given only to whoever picked the item that crossed the
  next extend threshold.** Should grant the extend to **both** players regardless of who
  took the last item. (Notes #3.) Touches the extend-threshold check in the score/point
  path; grant to P1 (shared struct) AND P2 (`s_p2Lives`, the field-swap+heal pattern).
- **B2 — Full power on P1 turns ALL potential power items into cherry items**, even when
  P2 is below full power — which starves P2's only power source. Fix: only do the
  power→cherry transform when **both** players are at full power. (Notes #2.) Find the
  item-spawn/transform decision (item-drop path, cherry doc / item-collect doc) and gate
  it on `min(P1power, P2power) == full` instead of P1 alone.
- **B3 — P2 never gets autocollect above the point-of-collection line, EVER** — not even
  on Extra, where everyone should autocollect regardless of power. (Notes #7.) The
  autocollect trigger (full-power line-cross, and the Extra-stage always-on rule) is
  keyed to P1 only; replicate the condition for P2 and credit to P2.

## P1 — bombs (mechanics)

- **B4 — P2 bomb does not clear bullets.** (Notes #1.) P2's bomb deploys but the
  bullet-cancel that a bomb normally triggers doesn't run for P2. Find the bomb→
  clear-bullets call on the P1 path and re-invoke it for P2 (mind the single border-ring
  slot caveat, handoff §5d — this is the bomb/spell bullet-clear, distinct from the
  cherry border).
- **B5 — Extra/Phantasm bosses: P2 bomb doesn't trigger their invincible spell form.**
  In vanilla you can't damage an Extra/Phantasm boss during a spell — they go invincible
  for its duration. P1 bombs respect this; **P2 bombs don't**, so Extra/Phantasm fights
  get cheesy. (Notes #8.) Make P2's bomb damage path honor the same boss-invincible-
  during-spell gate P1's does. (claude oversimplified it here, ideally P2 bombing should trigger same tranforming mechanism as P1 bomb. if turns out too hard, full boss invul during P2 bomb can do too)

## P2 — design decision (NOT a bug — confirm intent, then enforce)

- **D1 — Resurrection should require ≥1 extend; no free resurrection on the last life.**
  (Notes #9.) This **contradicts the currently-shipped mechanic** (handoff §5f: "free if
  the survivor has no spare extends" + last-life 1up drop). User now wants the EoSD rule:
  you can only revive a partner if you have a life to spend. Before changing, confirm
  this supersedes §5f, then remove the free-revive / guaranteed-last-life-1up paths.
  (message from me, the user. confirmed! but only about free revives. when player pichuuns on last life, he still should drop 1up)

## Confirmed working-as-intended (no fix wanted)

- Proximity-based enemy aiming is **fun** as-is (notes #4) — keep.
- Boss movement commanded by **P1 position** is fine (notes #5) — keep.

## Suggested nightshift order
C1/C2 (crashes) → B1/B2/B3 (resource sharing, high play-feel impact, low risk) →
B4/B5 (bombs) → D1 (needs the user's confirmation it supersedes §5f, so do last or ask).

---

## Status — 2026-06-16 (unattended session, PR #5)

All compile-verified (6 targets, 32-bit; native merge test passes). **No in-game test
available this session** — items below are reasoned from the decomp, not play-tested.

### ⚠️ CRITICAL FINDING — the x87 ST0 hook hazard (read before touching B1/B2)
Several of ZUN's resource/item fns read the **x87 FPU `ST0`** register that their
**caller** pushed, instead of taking it as a normal argument:
- `FUN_0042d83a` (extend grant — reads round(ST0)=lives, then bombs)
- `FUN_004326f0` (item spawner — `FUN_0048b8a0()` reads round(ST0)=power for the
  power→cherry auto-convert at PCBdecomp.c:20255)
- `FUN_0048b8a0` itself (`ROUND(in_ST0)`, ≈ `lroundf`) used all over the credit switch.

A **MinHook C detour cannot wrap these and call the original**: the detour's C
prologue clobbers `ST0` before the trampoline runs, so the original reads garbage.
(coop's existing hooks are safe only because their targets — `FUN_0043d9e0` damage,
`FUN_0043e260` collision, etc. — take real args, not `ST0`.) Implication: B1/B2 must
NOT be done by hooking those fns. B1 below uses a side-channel instead; B2 has no
clean binary-feasible route yet.

### ✅ D1 — no free revives — DONE (commit 28c66d7)
`ReviveByGraze` now requires the reviver to hold ≥1 spare extend; with none the
focus-release confirm does nothing (the channel stays charged so it fires the instant
they earn a life). The last-life-death **1up drop is kept** (per the user's note). The
P1/P2 paths are symmetric. Header comment updated.

### ✅ B1 — point-item extend shared — DONE (commit 28c66d7)
A point-item threshold extend (PCBdecomp.c case 1, the `+0x28 >= +0x30` loop calling
`FUN_0042d83a`) now grants the life/bomb to **both** players. Implemented FPU-safely
via the **extend-tier counter `res+0x2c`**: `CheckPartnerExtend()` watches it for an
increment (the 1up *item*, case 5, calls `FUN_0042d83a` WITHOUT bumping `+0x2c`, so
item pickups correctly stay collector-specific) and mirrors the extend to the partner
through the existing collect-time field-swap (the collector is named by `s_p2ResHeld`,
still set because the check runs before `RestoreHeldRes`). Partner gets a life if <8
else a bomb if <8 (ZUN's own overflow). Determinism-safe (no extra RNG; the heal is
the one RestoreHeldRes already does). **Verify in-game:** P1 and P2 both gain a life
when the point-item counter crosses an extend threshold, regardless of who collected.

### ✅ B3 — P2 autocollect above the line / on Extra — DONE (commit 28c66d7)
ZUN's per-item autocollect trigger (`FUN_00432990` PCBdecomp.c:20391) is
`(P1 full power || difficulty>3) && (P1_Y < line)`, P1-only. `ApplyP2Autocollect`
(in `HookedItemLoop`, after the original) replicates the same gate for P2 — when P2 is
above the collect line `*(*0x575948 + 0x20)` and (P2 full power || `*0x626280 > 3`),
it sets the homing-mode byte (`item+0x27f = 1`) on each active item for which P2 is the
nearer player. The engine's next-frame homing + the existing `HookedAngleToPlayer`
nearer-player redirect carry it to P2. Only writes the mode byte → no FPU/RNG, safe.
**Verify in-game:** P2 at full power above the line vacuums nearby items; on
Extra/Phantasm P2 autocollects regardless of power.

### ⏸ B2 — full-power→cherry only when BOTH full — DEFERRED (FPU hazard)
The conversion is in the item spawner `FUN_004326f0` (PCBdecomp.c:20255):
`if (round(ST0_power) > 0x7f && type∈{0,2}) type = 7`. It reads **P1's power off ST0**
— so per the hazard above it can't be C-hooked-and-forwarded, and there's no
non-FPU global to gate it on. Two viable routes, both needing what this session lacks:
1. **Binary patch** the convert site to read a DLL-controlled byte (needs the actual
   th07.exe bytes — not in the repo — and is untestable here).
2. **Hook + replicate**: hook `FUN_004326f0`, decide the final type in the detour from
   the *resource struct* power (P1) and `s_p2Power` (both-full ⇒ keep type 7, else pass
   0/2), and DEFEAT the engine's own convert (which will read the detour's clobbered
   ST0). Defeating it reliably needs an `fldz` immediately before the trampoline call
   (verify via `objdump` that no FPU op intervenes) AND confirming `FUN_0048b8a0` pops
   ST0 (else the x87 stack leaks one slot per spawn → eventual crash). Feasible but
   needs the disasm check + an in-game soak. **Do with the game in front of you.**

### 🟡 B4 — P2 bomb doesn't clear bullets — IMPLEMENTED, needs in-game test (commit pending)
**Shipped:** `HookedAddClearCircle` (FUN_004418b0) + `HookedAddClearBox` (FUN_00441800)
force ECX to P2 while `s_inP2Update` is set, so P2's bomb registers its clear-regions on
**P2+0x17dc** (where coop's existing `FUN_0043e260` re-invoke reads them). Safe-by-
construction: +0x17dc is bomb-only and during P2's update the only legitimate region
owner is P2 (no-op if ECX was already P2; fix if it was the P1 base). Default on
(`s_p2BombClear`). **Verify in-game:** P2's bomb now sweeps bullets the way P1's does,
for every character; P1's own bomb unchanged. If it does NOT clear after this, the
region positions are being computed from the P1 base too (not just ECX) — then also
log/redirect the pos arg. Mechanism RE below.


**The bomb's bullet clear is the per-player CLEAR-REGION array at `player+0x17dc`** —
NOT the global sweep (`FUN_00424740`/`FUN_004249a0`, which serve spell-declaration /
full-power-item / death / cherry-border, none of them player bombs) and NOT
`player+0x2400` (that's the cherry-border sweep timer). Full map (read this session):

- **Storage:** `player+0x17dc` = 96 clear-region slots, stride **0x20** (8 floats):
  `[0]=x [1]=y [2]=width(0 ⇒ circle) [3]=height [4]=radius(circle) [6]=? [7]=clear-type`,
  plus an int **timer at +0x18** (decremented per frame).
- **Register a region:** `FUN_004418b0` @0x004418b0 (circle: sets `[4]=radius`) and
  `FUN_00441800` @0x00441800 (box: sets `[2]=w,[3]=h`), both `__thiscall(ECX=player,
  pos*, …)` — they scan `player+0x17dc` for a free slot and write it.
- **Test/clear:** `FUN_0043e0a0` @0x0043e0a0 `__thiscall(ECX=player, bulletPos,
  bulletSize)` walks the 96 regions; a bullet inside any ⇒ returns 2. It's called at
  the top of the bullet HIT test `FUN_0043e260` (PCBdecomp.c:25992) — `if
  (FUN_0043e0a0(...)!=0) return 2` ⇒ the bullet update sets state 5 (clearing) +
  drops a scratch item. **coop ALREADY re-invokes `FUN_0043e260` for P2**
  (`HookedCollide`), so it reads `P2+0x17dc`.
- **Per-frame maintenance:** `FUN_00440940` @0x00440940 `__fastcall(player)` decays
  each region's `+0x18` timer and expires it.

⇒ The clear is **player-relative through and through**. So B4 reduces to ONE question:
when P2 bombs, do `FUN_004418b0`/`FUN_00441800` write the regions to **P2**+0x17dc?
The bomb callbacks (5708–6131, `__fastcall(player)`, e.g. ReimuA `FUN_00408710`) run
param-relative on P2 and call `FUN_004418b0(<ECX hidden>, param_1+0x930, …)`. Ghidra
hides the ECX, so it's unresolved whether the call threads ECX=param_1 (→ P2+0x17dc,
B4 would already work) or reloads the P1 static base `0x4bdad8` (→ regions land on
P1+0x17dc, P2's array stays empty → **exactly B4**). This is the documented coop ECX
pitfall (§5b).

**Decide it with the disasm** of `FUN_00408710`'s `FUN_004418b0` call (or instrument:
log `P2+0x17dc[0..]` after P2's bomb callback in `s_inP2Update`). **Scoped fix if
P1-hardwired:** detour `FUN_004418b0` + `FUN_00441800` and, while `s_inP2Update`,
force ECX to P2 (re-call with the P2 base) — small, safe, mirrors the existing
collision re-invoke pattern. No global over-clear, no FPU.

### ⏸ B5 — P2 bomb doesn't trigger boss invincible-spell form — NEEDS RE + TEST
RE this session: the "is a player spell/bomb active" predicate `FUN_0042ad66`
(@0x0042ad66) reads the **shared** resource struct field `res+0x1fbac` (the active
spell-card index; set to a spell id when a spell declares — PCBdecomp.c:16184 — and to
-1 when it ends — 16102/16132; `FUN_0042ad66` returns "active" when `>=0` or `==-2`).
Because it's the SHARED struct, B5 is NOT simply a P1-keyed read. So the next step is to
determine **whether P2's bomb launch actually declares a spell** (sets `res+0x1fbac`)
the way P1's does — i.e. does the bomb path call the spell-declare fn (~PCBdecomp.c:16170,
which writes `+0x1fbac = param_2`)? If P2's bomb sets it, the boss SHOULD go invincible
already and B5 may be a different gate (the boss's own per-spell invincibility flag, or
the damage path); if it does NOT, mirror the declare for P2.

The user-accepted fallback ("full boss invul during P2 bomb") is implementable in
`HookedDamage` (P2+0x16a20 bombing && enemy flags&0x40 boss && difficulty>3 ⇒ return 0
+ skip the P2 re-invoke) BUT it OVER-nerfs (blocks damage on attack spells where P1
could damage while bombing), so it risks regressing feel — left unimplemented pending
the proper gate + a play-test.

### ⏸ C1 / C2 — crashes — NEED IN-GAME REPRO
No new lead findable without a repro/log. C2's note says a `coop_log.txt` from that
session is in `build/` — but the committed one is stale (§5b). Capture fresh logs:
C1 = P1 Sakuya + P2 Reimu, Reimu's bullets hit an enemy; C2 = stage-4 death-fairy.
Both are plausibly in the per-player shot/aim graft (`BuildP2TargetBlock`, the
`HookedDamage` P2 re-invoke) — add player/shot identity logging to the hit path and
bisect by char combo.
