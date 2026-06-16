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
  **✅ FIXED 2026-06-16 (awaiting in-game confirm).** The convert lives in the item
  spawner `FUN_004326f0` (PCBdecomp.c:20255): it FLDs the shared power `res+0x7c` and
  rewrites a power item (type 0/2) to cherry (type 7) when `round(power) > 127`. The
  shared power is P1's. `HookedItemSpawn` now detours it: when P2 has separate power and
  is NOT full, it zeros the power the spawner reads across the original call (restoring
  after) so the item stays a power item — i.e. convert only when BOTH are full.
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
  **✅ FIXED 2026-06-16 (awaiting in-game confirm).** Mechanism: a bullet is cleared
  during a bomb when the graze test `FUN_0043e3b0` / hit test `FUN_0043e260` return **2**,
  which they do when `FUN_0043e0a0` finds the bullet inside the player's bomb-clear-region
  array at `player+0x17dc`. Both that array and `FUN_0043e0a0` are param-relative, so a
  P2-base call already tests P2's OWN regions (the bomb callbacks write them relative to
  the bombing player). The detours, though, dropped P2's return: `HookedGraze` only
  propagated `r2==1` (grazed) and `HookedCollideBullet` discarded P2's return entirely.
  Fix: propagate `r2==2` (clear) from P2 in both hooks (graze path = not-yet-grazed
  bullets, hit path = already-grazed ones). **The earlier "last attempt" likely chased the
  wrong mechanism** (the global bomb-active flags `DAT_004d44f8`/`DAT_004bfee0` are
  P1-struct fields at fixed addresses — P1+0x16a20 / P1+0x2408 — and do NOT drive the
  bullet clear; that is the `+0x17dc` region test).
- **B5 — Extra/Phantasm bosses: P2 bomb doesn't trigger their invincible spell form.**
  In vanilla you can't damage an Extra/Phantasm boss during a spell — they go invincible
  for its duration. P1 bombs respect this; **P2 bombs don't**, so Extra/Phantasm fights
  get cheesy. (Notes #8.) Make P2's bomb damage path honor the same boss-invincible-
  during-spell gate P1's does. (claude oversimplified it here, ideally P2 bombing should trigger same tranforming mechanism as P1 bomb. if turns out too hard, full boss invul during P2 bomb can do too)
  **✅ FIXED 2026-06-16 via the accepted fallback (awaiting in-game confirm).** ZUN gates
  ALL player shot damage on `DAT_004d44f8==0` (= P1 NOT bombing, PCBdecomp.c:12829) — that
  is the "invincible spell form": during P1's bomb the boss takes nothing. The flag is
  P1+0x16a20, never set by P2's bomb, so the boss kept taking P1+P2 damage during a P2
  bomb. `HookedDamage` now zeroes the sweep's damage while P2 is bombing (read P2+0x16a20),
  scoped to Extra/Phantasm (difficulty 4/5, `DAT_0062f85c`) to leave normal stages alone.
  The sweep still runs so shots are consumed + spark, exactly like ZUN's gate (which blocks
  only the apply). NOTE: this is the "full boss invuln during P2 bomb" fallback, NOT the
  exact P1 transform — left as a follow-up. The bomb→boss damage path is `FUN_0043d9e0`
  (the only caller is the enemy update at 12822); no separate bomb-damage path was found.

## P2 — design decision (NOT a bug — confirm intent, then enforce)

- **D1 — Resurrection should require ≥1 extend; no free resurrection on the last life.**
  (Notes #9.) This **contradicts the currently-shipped mechanic** (handoff §5f: "free if
  the survivor has no spare extends" + last-life 1up drop). User now wants the EoSD rule:
  you can only revive a partner if you have a life to spend. Before changing, confirm
  this supersedes §5f, then remove the free-revive / guaranteed-last-life-1up paths.
  (message from me, the user. confirmed! but only about free revives. when player pichuuns on last life, he still should drop 1up)
  **✅ DONE 2026-06-16.** `ReviveByGraze` now revives ONLY when the reviver has a spare
  life to donate (no free revive); the graze channel completes but nothing happens with
  zero spares. The guaranteed last-life 1up drop is KEPT (it lives in EnterGhostP1/P2 on
  death, separate from the revive path) per the user's note.

## Confirmed working-as-intended (no fix wanted)

- Proximity-based enemy aiming is **fun** as-is (notes #4) — keep.
- Boss movement commanded by **P1 position** is fine (notes #5) — keep.

## Suggested nightshift order
C1/C2 (crashes) → B1/B2/B3 (resource sharing, high play-feel impact, low risk) →
B4/B5 (bombs) → D1 (needs the user's confirmation it supersedes §5f, so do last or ask).
