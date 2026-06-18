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

### 🟡 B2 — full-power→cherry only when BOTH full — IMPLEMENTED (naked ST0 trampoline), needs in-game soak
**Done** via the FPU-safe workaround the prior note flagged as route 2, but realized as
a **naked detour** rather than a C-hook-and-forward (cleaner — it never runs a C
prologue, so ST0 is never clobbered to begin with). `HookedItemSpawn` (coop.c) is
`__attribute__((naked))`: it inspects the type arg at `[esp+8]` and, only for the
convertible types (0,2) and only when `g_b2Suppress` is set, does `fstp %st(0); fldz`
— popping the caller's P1 power and pushing 0.0 so the engine's own
`round(ST0) > 0x7f` test fails and the item stays a power item. Everything else
(including coop's own type-5 1up drops) is a clean `jmp` to the trampoline with ST0
untouched. **Depth is preserved** (pop 1 + push 1 = the one slot `FUN_0048b8a0` pops),
so no x87 stack leak — which resolves the prior note's open worry: `FUN_0048b8a0` is
the standard lround helper (PCBdecomp.c:81531, `ROUND(in_ST0)`) and *does* pop, else
vanilla would leak one slot per spawn and crash in seconds (it doesn't).

`g_b2Suppress` is computed in C, FP-safely, once per frame in `HookedUpdate`:
`s_p2 && s_p2Power < COOP_FULL_POWER` (P1 full + P2 not full ⇒ keep power; both full ⇒
leave ST0 alone ⇒ vanilla converts). **Gated** behind `coop.ini [coop]
cherry_both_full` (default 0) and the hook is only *installed* when the flag is set, so
a default build is byte-for-byte unchanged.

**Verify in-game (local co-op):** P1 at full power, P2 below full — killed enemies keep
dropping **power** items (not cherries) until P2 also tops out; once both are full,
drops convert to cherry as in vanilla. Soak a full stage to confirm no x87 drift
(item spawns stay correct over hundreds of drops; no crash). **Net caveat:** it reads
`s_p2Power`, the value currently desyncing — keep `cherry_both_full=0` over the network
until power sync is solid, or it will feed the diverging value into drop types.

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

### 🟡 B5 — P2 bomb → Extra/Phantasm boss invuln — IMPLEMENTED (fallback), needs in-game test
Ported from nightshift's keen-ramanujan branch (commit 2a5c785) to main. ZUN gates ALL shot
damage on `DAT_004d44f8==0` (= P1 NOT bombing, PCBdecomp.c:12829), so during P1's bomb the boss
takes nothing (its invincible spell form). That flag is P1's field (`P1+0x16a20` = `OFF_BOMBING`);
P2's bomb never set it, so the boss kept taking P1+P2 damage during a P2 bomb. Fix in
`HookedDamage`: when P2 is bombing (`P2+OFF_BOMBING != 0`) on Extra/Phantasm (`ADDR_DIFFICULTY`
4/5), zero the sweep's damage — the sweep already ran so shots are still consumed/sparked, exactly
like ZUN's gate (which blocks only the apply). This is the **user-accepted fallback** ("full boss
invuln during P2 bomb"); reproducing P1's exact transform is the follow-up. **Verify in-game:**
on Extra/Phantasm, P2 bombing during a boss spell stops damage (no cheese); normal stages
unaffected. NB: uses main's validated `ADDR_DIFFICULTY 0x626280`, not the branch's `0x62f85c`.

### ⏸ B5 (original RE notes) — the proper transform follow-up
RE this session (corrects an earlier guess): `+0x1fbac` is the **BOSS** spell-card
index, not the player bomb — `FUN_00429a4f` @0x00429a4f `__thiscall(scoreMgr, spellIdx)`
declares a boss spell (writes `+0x1fbac = spellIdx`, resets the 900-dword spell-bonus
block); cleared to -1 at spell end (16102/16132). `FUN_0042ad66` @0x0042ad66 reports
"a spell is active" off this. So the boss invincibility tied to spells is keyed to the
**boss's** state, and B5 ("P2 bomb doesn't make the boss go invincible") is most likely
in **how the player bomb signals the boss damage path** — i.e. a "player is bombing →
deny boss damage during this spell" gate that reads P1's bomb (`FUN_0042ad66` with
ECX=P1, or `P1+0x16a20`) but not P2's. Next: find where boss/enemy damage
(`FUN_0043d9e0` / the enemy damage-apply `FUN_00420620`) is suppressed during a spell
while bombing, and check whether it consults P2's bomb. That gate is the real fix
point; mirror it for P2 (`P2+0x16a20`).

The user-accepted fallback ("full boss invul during P2 bomb") is implementable in
`HookedDamage` (P2+0x16a20 bombing && enemy flags&0x40 boss && difficulty>3 ⇒ return 0
+ skip the P2 re-invoke) BUT it OVER-nerfs (blocks damage on attack spells where P1
could damage while bombing), so it risks regressing feel — left unimplemented pending
the proper gate + a play-test.

### ⏸ C2 — crash — NEED IN-GAME REPRO
No new lead findable without a repro/log. C2's note says a `coop_log.txt` from that
session is in `build/` — but the committed one is stale (§5b). Capture a fresh log:
C2 = stage-4 death-fairy. Plausibly the same anm-table family as C1 (below) or the
per-player shot/aim graft (`BuildP2TargetBlock`) — but confirm with a repro.

---

## Status — 2026-06-17

### ✅ C1 — P1 Sakuya + P2 Reimu crash — ROOT CAUSE FOUND + FIXED (needs in-game confirm)
**Root cause: a missing `SwapAnm` around the P2 damage re-invoke in `HookedDamage`.**
The shot-damage sweep `FUN_0043d9e0` (which coop re-invokes param-relative for P2,
since the engine hardwires ECX=P1) **rebinds a shot's anm on hit** at
`PCBdecomp.c:25902`:
`FUN_0044ea20(shot, *(anmMgr + 0x28ef0 + (shot[+0x1d8] + 0x20) * 4))` — i.e. it indexes
the **0x400-range script table that `SwapAnm` owns**, using the shot's hit-variant id.
coop's P2 re-invoke wrapped that call in `SwapSelGlobals` but **not `SwapAnm`**, so for a
**different-character P2** the table still held P1's char scripts. P1 Sakuya / P2 Reimu
then indexes **Sakuya's** table with **Reimu's** hit-id → binds a wrong/garbage script
pointer into the shot → **intermittent crash a few frames later when that script runs**.

This explains every reported property:
- **char-combo-specific** — only a *different*-char P2 sets `s_p2AnmActive=1` (same-char ⇒
  `SwapAnm` is a no-op ⇒ no bug, matching "other combos fine"); the Sakuya-vs-Reimu id
  layout is what makes Reimu's hit-id land on a fatal entry in Sakuya's table.
- **intermittent / "crashes eventually"** — the bad bind only detonates when the wrong
  script does something fatal, which depends on later frames/state.
- **"sometimes bullet hit, sometimes bomb"** — Reimu's amulets *and* her bomb orbs both
  damage enemies through this same `FUN_0043d9e0` sweep, so both hit the same rebind.

**Fix (coop.c `HookedDamage`):** wrap the P2 re-invoke in `SwapAnm(1)/SwapAnm(0)` too,
mirroring the player update + draw (which already wrap their P2 engine calls in both
`SwapSelGlobals` *and* `SwapAnm`). No-op for a same-char P2, so default play is unchanged.
Builds clean (6 targets, 32-bit). **Verify in-game (local co-op, no netplay needed):**
P1 Sakuya + P2 Reimu (either shots), play a stage firing P2 into enemies + bombing —
the combo that crashed should now be stable. Then spot-check the other different-char
combos still behave.

### ℹ B2 — cherry-power — IS implemented, but OFF BY DEFAULT (likely why it "wasn't fixed")
The B2 power→cherry-only-when-both-full fix is present and logic-verified (the spawner
`FUN_004326f0` calls the lround helper `FUN_0048b8a0()` **once**, using ST0 only for the
convert test — so the naked trampoline's ST0→0.0 swap is safe). BUT it is **gated behind
`coop.ini [coop] cherry_both_full` (default 0), and the hook is only *installed* when the
flag is set** — so a default build does nothing. It was kept off because it reads
`s_p2Power`, which was desyncing over the network; that desync (x87) is now CONFIRMED
FIXED (2026-06-17), so the reason to keep it default-off is gone. **Decision (user,
2026-06-17): flip the default ON.** `cherry_both_full` now defaults to 1 (coop.c static
init + the `GetPrivateProfileIntA` fallback + the build.ps1 ini template); set it to 0 to
restore vanilla. Still wants an in-game soak — B2 has never been play-confirmed, only
reasoned — so verify a full stage of drops while P1 is full / P2 isn't (items stay power)
and once both are full (drops convert to cherry).

> **⚠️ DEPLOY CAVEAT (the "B2 still doesn't work" report, 2026-06-17):** the default flip
> only applies if `cherry_both_full` is ABSENT from the deployed `coop.ini` —
> `GetPrivateProfileIntA` can't tell "absent" from "0". The test logs show
> `cherry_both_full=0` because the users' deployed `coop.ini` (older template) sets it
> explicitly. **Action: set `cherry_both_full = 1` (or delete the line) in coop.ini on BOTH
> machines.** That session was netplay (`net.enabled=1`); B2 reads `s_p2Power`, fine now
> post-x87-fix.

### ✅ B4 — P2 bomb doesn't clear bullets — ROOT CAUSE FOUND + FIXED (needs in-game confirm)
The clear-region *registration* (`FUN_004418b0`/`00441800` → `P2+0x17dc`) was already
working; the gap was on the READ side. The per-bullet loop (`PCBdecomp.c:14718`) runs the
**graze test `FUN_0043e3b0`** — not the hit test `FUN_0043e260` — for every **un-grazed,
older bullet**, and that graze test *also* does the clear-region check (`FUN_0043e0a0`) and
**returns 2 when the bullet is in a clear region**. `HookedGraze` only forwarded P2's result
when `r2==1` (grazed) and **dropped `r2==2`**, so bullets in P2's bomb field but not near
P2's graze box were never cancelled. Matches the report exactly: "balls don't clear bullets,
except the rare one P2 also grazed" (that one took the hit-test path, which DOES handle 2,
and dropped a scratch item → the "turned into stars once").

**Fix (`HookedGraze`):** forward `r2==2` (cancel), priority 2 > 1 > 0. Reads only synced sim
state (P2 regions + position) ⇒ lockstep-safe. Builds clean. **Verify in-game:** P2's
focused-bomb balls now cancel bullets they pass over, like P1's. NB: ZUN's field-cancel drops
a small type-1 scratch item per bullet for BOTH players — vanilla, not a P2-only artefact.

### 🔬 B5 — P2 bomb armour "does not work at all" (Extra) — INSTRUMENTED, awaiting log
Logic looks right (zero the damage return while P2 bombing on Extra/Phantasm ⇒ ZUN's
`if (0 < local_1c)` apply at `:12827` is skipped). Two suspects: (a) the difficulty global —
B5 reads `0x626280`, but the boss-damage code uses `DAT_0062f85c` for its own Extra/Phantasm
rules (`:12849`, values 4/5/6/8, not the clean 0–5 ladder `0x626280` holds); (b) P2's bombing
flag (`P2+0x16a20`) timing. Added an **edge-triggered diagnostic** in `HookedUpdate`: logs
both difficulty globals + the bombing flag at each P2 bomb START, and at bomb END the count
of times B5 zeroed boss damage. `count==0` ⇒ condition never matched (difficulty/bombing read
wrong); `count>0` but boss still died ⇒ damage isn't flowing through this return. **Next: one
Extra run, P2 bomb on a boss, then read the `B5 diag:` lines in `coop_log.txt`.**

### Confirmed working this session (2026-06-17 test)
- **C1** (Sakuya+Reimu crash) — no longer crashes. ✅
- **C2** (stage-4 death-fairy crash) — did not recur. ✅ (likely the same anm-table family as C1)
- **B1** (shared point-item extend) — works. ✅
- **B3** (P2 autocollect above the line) — works on a normal stage; Extra not yet confirmed.
- P2 shots feed the team cherry counter (they live in P1's shot array) — confirmed expected.

### New backlog from the 2026-06-17 test (unprioritised — confirm with user before acting)
- **N1 — revive/life-transfer depends on shooting state.** User couldn't transfer a life while
  P1 was shooting, but could while P2 was shooting. Intended (D1/§5f): graze 90f then
  focus→unfocus, independent of fire. Look at `ReviveByGraze` for any read of the shoot bit /
  a second confirmation barrier; it may be post-revive-only. Possibly just-after-revive state.
- **N2 — damage damper feel.** Current `r*=0.6` applies to ALL enemies, making stage popcorn
  tanky (one player can't solo the Extra opening ball line). User floated boss-only and/or 0.75
  but is undecided (tankier stage enemies may be fine / even fun). Design call — don't change yet.
- **N3 — Alice penultimate spell + first Prismriver spell "look different in co-op"** (less
  structured bullet rings than expected). Could be difficulty confusion (Hard vs Lunatic memory)
  or a real RNG/object-order effect from the P2 graft. Worth a determinism spot-check; low
  priority until reproduced deliberately.
- **N4 — Prismriver fight** currently follows P1 (single boss). User mused about a 2-vs-2 variant
  later; "make it work first." No action.

## Status — 2026-06-17 (session 3 — retest)

Retest results: **B5 (Extra bomb armour) now WORKS** ✅. **B4 "kinda works"** (the focused-bomb
clear now fires) — one residual bug below. **B2 STILL off** — found the real cause (launcher).

### ✅ B2 — cherry-power — ROOT CAUSE was the LAUNCHER (fixed)
The CONFIG log read `cherry_both_full=0` again because **the launcher itself stamped `=0`** into
every deployed coop.ini: `WriteIni` had a stub that wrote `"0"` when the key was absent
(`launcher.c`, old lines 105-108). So the DLL default-flip never mattered. **Fix:** replaced the
stub with a real **"Cherry power" checkbox** (default ON for a fresh ini), written every launch
like the other boxes. Also added DLL diag counters (`g_b2Calls`/`g_b2Suppressed`) — the detour
now logs `B2: detour FIRING …` the first time it keeps a drop as power, so install+fire is
self-evident in the log. **Action for the existing testers:** their ini already has
`cherry_both_full=0`, so the new checkbox loads UNTICKED — tick it once (or delete the line) and
relaunch; CONFIG should then show `cherry_both_full=1` and the `B2: detour FIRING` line appear.

### ✅ N2 — damper mode — DONE (launcher checkbox)
New `coop.ini [coop] damper_boss_only` + launcher box **"Damage cut: bosses only"**. OFF (default)
= flat `COOP_DAMPER_FLAT` 0.75 on every enemy (was 0.60). ON = only lifebar enemies (boss/midboss,
`enemy+0x2e29` bit6) take `COOP_DAMPER_BOSS` 0.60; stage popcorn takes full damage. Boss-ness read
from the snapshot offset HookedDamage already computes (`pos - ENEMY_OFF_POS`). Constants are easy
to retune. `s_bossHpScale` (F5) still master-gates the whole damper.

### 🐛 B4b — SakuyaB unfocused (timestop) bomb leaves live leftover bullets — FOLLOW-UP
P2's Sakuya timestop clears most bullets but leaves a few with **active hitboxes after the bomb
ends**. Mechanism traced: the timestop's bullet wipe is a **screen-wide clear region registered at
bomb-END** (`FUN_004418b0(player+0x930, r=800, …, timer=0)`, `:7468`) on `player+0x17dc`. That
region lives ~1 frame (`FUN_00440940` expires timer-0 regions). The player update `FUN_00441fb0`
runs region-maintenance then the bomb handler, and **early-returns while the timestop flag
`DAT_0062627c` is set** (`:27212`). The leftover is a P2-vs-P1 timing asymmetry (when P2's
screen-wide region is registered/expired vs when the global bullet loop tests it, across the
timestop release) — needs **in-game instrumentation** (log `P2+0x17dc[0]` radius + `DAT_0062627c`
across a P2 SakuyaB bomb) before a safe fix. Not the common-case B4 path (that's fixed).

### Decisions/Notes this session
- **B5 diagnostic** left in (edge-triggered, cheap) until armour is confirmed stable across runs.
- **N1** (revive vs shooting) — user: non-critical, leave for now.
- **N3/N4** — user attributes N3 to the proximity-aim graft; both tabled (N4 alongside 3P).

## Status — 2026-06-17 (session 4)

Two user reports: (1) **B5 bomb armour over-fires** — it denies damage "anytime": when no
boss is present, and when the boss is on a *nonspell* attack; it should apply ONLY while the
boss is on a spell card. (2) **B2 cherry-power "still ongoing — config is right, behaviour is
the same."** Both root-caused from the binary (not just the decomp) and fixed.

### ✅ B5 — bomb armour now gated on an ACTUAL boss spell card
The first cut zeroed damage on **any** Extra/Phantasm P2 bomb, so it also nullified stage
popcorn and a boss between/without cards. Real fix: gate on **(a)** the damaged enemy being a
boss (`enemy+0x2e29` bit6, the flag the damper already reads off `pos - ENEMY_OFF_POS`) **and
(b)** a boss spell card actually being active. ZUN's own "spell active?" predicate is
`FUN_0042ad66(ECX=0x0049fbf0)` → reads the boss spell-card index at `*(0x0049fbf8) + 0x1fbac`;
active iff index `>= 0` (a card) or `== -2` (the brief setup/transition state). **Verified
in-binary:** all nine `FUN_0042ad66` call sites do `mov ecx,0x49fbf0`, body reads
`[[ecx+8]+0x1fbac]`. coop reads the field directly via `BossSpellActive()` (no ZUN call, no
side effects). The B5 START diag now also logs `spell_active`. **Verify in-game (Extra):** P2
bomb during a boss **spell** stops boss damage; P2 bomb on a **nonspell**, or with no boss,
deals normal damage; stage popcorn is never immune.

### ✅ B2 — cherry-power — REAL root cause: the naked ST0 detour was a no-op
"Config is right, behaviour the same" because the fix never did anything — even with the hook
installed and firing. The decomp models the convert as `FUN_0048b8a0()` reading `in_ST0`, which
led to the assumption that the **caller** pushes P1's power on the x87 stack (the "ST0 hazard"),
hence the naked `fstp/fldz` trampoline. **The disassembly of `FUN_004326f0` disproves it** — the
function loads P1's power **fresh from memory** and ignores the caller's ST0:
```
432710  mov eax,[0x626278]        ; res base (== *ADDR_RES_PTR)
432715  fld dword ptr [eax+0x7c]  ; ST0 = P1 power (RES_POWER)   <- from MEMORY, not caller
432718  call 0x48b8a0             ; round(power)
43271d  cmp eax,0x80 ; (>=128) && type∈{0,2} ⇒ type=7 (cherry)
```
So `432715` overwrote whatever the old detour left in ST0 → suppression never happened. **Fix:**
replaced the naked detour with a **normal thiscall C detour** (safe now — the function takes no
ST0 input). When suppressing (`g_b2Suppress`: P2 live & below full) and the item is convertible
(type 0/2), it temporarily writes `*(res+0x7c)=0.0f` around the original call so the engine's own
`round(power)>=0x80` test fails, then restores it. 0.0f is all-zero bits ⇒ save/zero/restore is a
plain integer memory copy: **no FPU, no RNG, deterministic**, checksum (`res+0xb0`) untouched
(restored before any accessor runs). `432715` is the spawner's **sole** power read (verified), so
nothing else is disturbed. **Verify in-game:** P1 full / P2 below full ⇒ kills keep dropping
**power** (not cherry); once both full ⇒ cherry as vanilla. The `B2: detour FIRING …` log line
should now appear (and `cherry_both_full=1` in CONFIG).

> The earlier `docs/.../th07_coop_gameplay_bugs.md` ST0-hazard warning (PR #5 §"CRITICAL FINDING")
> still holds for `FUN_0042d83a` / `FUN_0048b8a0` *callers* that genuinely push ST0 — but
> `FUN_004326f0` is **not** one of them; it self-loads from memory.

## Status — 2026-06-18 (session 5)

Retest: **B2 cherry-power CONFIRMED WORKING** ✅ (user: "finally works"). **B5 regressed the
other way** — now P2 bombs damage the boss *always* (two bombs killed Ran's spell). Plus a
request: add logging to help debug future cryptic crashes.

### 🔬 B5 — spell gate reads "no spell" during a real spell — instrumented, awaiting a log
The `B5 diag: P2 bomb START … spell_active=0` line printed **0 on every bomb**, and the
`bomb END … zeroed = 0` count confirms the gate never engaged across the whole bomb — so
`BossSpellActive()` returned false even while the boss was visibly mid-spell. The reimplemented
read (`*(0x0049fbf8)+0x1fbac`) is byte-identical to ZUN's `FUN_0042ad66`, so either (a) a subtle
difference in our hand-read vs the real call, or (b) the START snapshot is misleading and the
field genuinely isn't ≥0 at the damage frames. Changes this session:
- `BossSpellActive()` now **calls ZUN's own `FUN_0042ad66(ECX=0x0049fbf0)`** (a pure predicate,
  the exact one the boss-healthbar code at PCBdecomp:13065 uses) instead of re-reading the field
  — removes all doubt about the pointer indirection.
- Added a **decision-point diagnostic**: the first time a *boss* is damaged during a P2 bomb,
  `B5 diag@hit:` logs `spellFn` (ZUN's result), `idx` (raw `+0x1fbac`), the enemy flag byte
  (`b3invuln/b4dmg/b6boss`), and `r`. One Extra/Phantasm run with a P2 bomb on a boss spell will
  show exactly what the spell index reads at the moment damage flows.
- Context (user): Extra/Phantasm bosses **transform into an invincible shiny ball when avoiding
  damage** (you bomb during a spell). That ball-invuln is what P2's bomb must mimic; it's the
  same spell-active condition, just visualised. **Next:** read the `B5 diag@hit:` line — if
  `idx >= 0` but `spellFn=0` something's inconsistent; if `idx == -1` at the hit, `+0x1fbac` isn't
  the live signal for this fight and we need the per-boss invuln state instead.

> NB difficulty in the logs: `diff[0x626280]=5` (=Phantasm per the 0–5 ladder; 4=Extra) and
> `diff[0x62f85c]=8`. The B5 gate accepts 4||5 so difficulty isn't the blocker.

### ✅ Crash logging — vectored exception handler + hook breadcrumbs (added)
For the cryptic C1/C2-family crashes (fault lands inside a ZUN fn we re-invoked for P2, frames
after the cause), added:
- **`CoopCrashHandler`** via `AddVectoredExceptionHandler` — on a fatal exception (AV, illegal/
  priv instruction, in-page, div0, stack overflow) it logs to `coop_log.txt`: exception code,
  faulting **EIP**, read/write/execute **target address**, all GP registers, and the coop
  breadcrumb, then `EXCEPTION_CONTINUE_SEARCH` (never swallows the crash). No ASLR ⇒ a raw EIP
  like `0x0043d9e0` greps straight into PCBdecomp/objdump.
- **Breadcrumbs** (`s_crumb`/`s_crumbWho`/`s_crumbSeq`, `CRUMB()`/`CRUMB2()`): set at each hot
  hook entry (update/draw/damage/collideBullet/collideLaser/graze) **and around every P2
  re-invoke** (e.g. `damage:P2-reinvoke` — the C1 anm-rebind hotspot). So the crash line says
  *which* hook and *which player* (P1 vs P2 re-invoke) was running. Essentially free (a pointer +
  int per hook entry).
- Caveat: the VEH fires process-wide; it's filtered to fatal codes, but if PCB/D3D ever raise a
  *handled* first-chance AV we'd log a spurious `CRASH` block. Old ZUN games don't do that in the
  game loop, so it should be clean — if noise appears, switch to `SetUnhandledExceptionFilter`.
- Further logging we could add on demand: per-frame P1/P2 state heartbeat, the SwapAnm id-table
  mapping at bind time, and enemy/shot pool occupancy. Held back as noisier; add if a crash needs
  it.

## Status — 2026-06-18 (session 6) — B5 spell signal CORRECTED via the EoSD decomp

The `@hit` diagnostic settled the B5 mystery: during a P2 bomb that killed a boss spell, the log
read `scActive`/`idx` for the score-manager declaration index as **-1** and ZUN's own
`FUN_0042ad66` returned **0** — i.e. the field we'd gated on (`*(0x0049fbf8)+0x1fbac`) is the wrong
one. The user pointed at the clean-room **EoSD decomp** (`d:/PCB Co-op/th06_multi_net`), which
names the real mechanism:

- `g_EnemyManager.spellcardInfo.isActive` — set `=1` by the ECL spellcard opcode at spell start
  (`EclManager.cpp:731`), `=0` at end. This is "a boss spellcard is being attacked."
- `spellcardInfo.usedBomb` — the bomb handler does `usedBomb = isActive` (`Player.cpp:364`),
  marking "a bomb was used this spell."
- Boss spellcard **damage model** (`EnemyManager.cpp` ~626, identical to PCBdecomp.c:12867-12899):
  `if (isActive) { if (out==0) dmg/=7; else if (usedBomb) dmg/=3; else dmg=0; }`. The **`dmg=0`**
  leaf is exactly the user's "boss transforms into an invincible ball / removes its hitbox."

PCB maps these to `DAT_009545c8` (isActive) and `DAT_009545dc` (usedBomb) at `[param_1 + …]`;
param_1 (enemy-manager context base) = 0 for the single active game, so the absolute globals
**0x009545c8 / 0x009545dc** are correct (the addresses Ghidra resolved). coop.c now reads them as
`SpellcardActive()` / `SpellcardUsedBomb()`.

**B5 now gates on `SpellcardActive()`** (was the dead `+0x1fbac`), still `&& isBoss && P2 bombing
&& diff∈{4,5}` → zero the sweep return. The `B5 diag@hit:` line now logs `scActive usedBomb declIdx
flags r enBase`. **Confirm next run:** `scActive` should be nonzero while bombing a boss spell (and
the armour engages, `bomb END … zeroed > 0`); `declIdx` stays -1. If `scActive` is 0 on a real
spell, param_1≠0 and we resolve the enemy-manager base from `enBase`.

> The earlier session-5 crash was unrelated to B5 — it was bombing while **P2 killable was off**
> (a separate bug to chase). The `FUN_0042ad66` call was reverted to a plain memory read regardless.

## Status — 2026-06-18 (session 7) — B5 base FIXED, focus-hitbox fade, menu net HUD

**B5 — the "param_1 = 0" assumption was WRONG; armour now reads the LIVE enemy-manager base.**
The session-6 `@hit` dump came back exactly as the fallback note predicted: `scActive=0
usedBomb=0 … enBase=0x009b3998` while a real Extra boss spell was being bombed, and `bomb END …
zeroed = 0`. `enBase` (the boss ENEMY) is non-zero, which means `param_1` (the enemy-MANAGER
context base) is non-zero too — so `0x9545c8` / `0x9545dc` are **displacements off that base**,
not absolute globals. Reading the absolute addresses returned a perpetual 0, so the armour never
engaged.

Fix: `param_1` is the `ECX` of the enemy-update fn `FUN_00420620` (it stores ECX at `[ebp-0x1e8]`,
the exact local feeding the spellcard read at `0x421387` / PCBdecomp.c:12868). `HookedEnemyUpdate`
captures that base into `s_enemyMgr` every tick (record-and-forward, no behaviour change), and
`SpellcardActive()`/`SpellcardUsedBomb()` read `s_enemyMgr + 0x9545c8 / +0x9545dc`. The hook was
**defined but never installed** before — it's now in the `MH_CreateHook`/`MH_EnableHook` list
(`ADDR_ENEMY_UPDATE`). The `B5 diag` START/@hit lines now also print `mgr=0x%08x`. **Confirm next
run:** `mgr` is non-zero, `scActive=1` while bombing a boss spell, `bomb END … zeroed > 0`.

**Gameplay — proximity fade now fades the OTHER player's focus-mode hitbox too.** The fade only
tinted the sprite (`player+OFF_TINT`); the focus ring + central hitbox dot (effect type `0x18`,
handle at `player+0x9d8`) stayed fully opaque and, per the user, is *more* distracting than the
sprite when the two overlap. `FadePlayerFocusFx()` writes the same alpha into the effect's ARGB
field (`fx+0x1b8`, set once at spawn by `FUN_0041c610`, never rewritten by the focus updater
`FUN_0041abe0`, so a per-frame write sticks to the draw). Applied symmetrically in
`ApplyProximityFade` (host fades P2's, guest fades the remote P1's). No-op when that player isn't
focusing. Gated on `proximity_fade` exactly like the sprite fade.

**Netplay — status HUD now shows on the menu (top row).** Menu desyncs were invisible until the
game refused to start in sync. `HookedSceneTick` runs the same lockstep merge on the front-end, so
`s_netFrame` / `s_netSync` / the RNG oracle are live there. `DrawNetMenuStatus()` queues a top-row
ascii line — `NET <H|G> F<frame> d<delay> <SYNC|DSYN> <selfRng>/<rcvRng>` (or `NET … WAITms` on a
stall, `NET LOST` on drop) — via the same global ascii queue the menu already flushes for the "P2
SELECT CHARACTER" prompt. Pure draw, no determinism impact. A menu desync is now visible the instant
the two RNG seeds diverge. (Menu *stability* itself is still open — see NETPLAY_TEST notes.)
