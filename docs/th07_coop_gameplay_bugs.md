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
  during-spell gate P1's does.

## P2 — design decision (NOT a bug — confirm intent, then enforce)

- **D1 — Resurrection should require ≥1 extend; no free resurrection on the last life.**
  (Notes #9.) This **contradicts the currently-shipped mechanic** (handoff §5f: "free if
  the survivor has no spare extends" + last-life 1up drop). User now wants the EoSD rule:
  you can only revive a partner if you have a life to spend. Before changing, confirm
  this supersedes §5f, then remove the free-revive / guaranteed-last-life-1up paths.

## Confirmed working-as-intended (no fix wanted)

- Proximity-based enemy aiming is **fun** as-is (notes #4) — keep.
- Boss movement commanded by **P1 position** is fine (notes #5) — keep.

## Suggested nightshift order
C1/C2 (crashes) → B1/B2/B3 (resource sharing, high play-feel impact, low risk) →
B4/B5 (bombs) → D1 (needs the user's confirmation it supersedes §5f, so do last or ask).
