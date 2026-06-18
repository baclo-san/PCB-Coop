# PCB-Coop — Touhou 7 (Perfect Cherry Blossom) co-op netplay

Adds 2-player (eventually 3) co-op netplay to `th07.exe` via **runtime DLL injection**
(no source for PCB exists), modeled on the EoSD mod
[RUEEE/th06_multi_net](https://github.com/RUEEE/th06_multi_net). Delay-based lockstep:
one 16-bit input word carries both players (P1 = low 9 bits, P2 = high 7), kept in sync
by an RNG-seed comparison every frame.

**Target binary:** `th07.exe` ver **1.00b**, SHA256
`35467EAF8DC7FC85F024F16FB2037255F151CEFDA33CF4867BC9122AAA2E80CA` (650,752 bytes).
**Every address in this repo is build-specific to that hash.**

## Start here
- **`docs/th07_netplay_handoff.md`** — the canonical design doc / session handoff. Read
  §0–§1 (addresses), then the latest dated §5x progress section and its next steps.
- **`docs/th07_fork_a_integration.md`** — verified hook seams for wiring the netcode
  into the game (input-inject + seed-sync detours, menu lockstep, the coop.c wiring plan).
- **`docs/th07_boss_hp_scaling.md`** — Tier-1 boss/enemy HP scaling (implemented; plus
  an alternative damage-side lever).
- **`docs/th07_cherry_determinism.md`** — the three-value cherry model, the cherry↔
  item-drop RNG coupling, and the shared-border design (game-tested).
- **`docs/th07_item_collect_credit.md`** — verified item-collection credit map
  (power/bombs/points/1up) + the per-player resource attribution design.
- **`docs/th07_player_struct.md`** — verified PCB Player-object offset map + the death
  FSM / resurrection seam (for the Fork-B second-player graft).
- **`docs/th07_hud_sprite_system.md`** — the HUD/score-manager draw, the anm
  sprite-object layout + blit (`FUN_0044f770`), the sprite-batch flush, and the
  ascii text queue — the ground truth for drawing sprites/text from the DLL.
- **`docs/th07_bullet_system.md`** — the enemy-bullet (danmaku) manager, per-bullet
  struct map, spawn/update/draw fns, and the graze→hit collision gate that Fork B
  rides.
- **`docs/th07_enemy_system.md`** — the enemy manager/array, per-enemy struct,
  the ECL VM tick + death flow, and the shot-damage sweep co-op scales.
- **`docs/th07_ecl_vm.md`** — the ECL danmaku-VM opcode table (0–161) + variable
  registers (the one fn Ghidra couldn't decompile), cross-referenced to the repo's RE.
- **`docs/th07_player_shot_bomb_system.md`** — the player shot-fire gate, the
  `.sht` buffers + baked stats, the per-shot slot struct (offsets confirmed via
  the damage sweep), and the bomb / cherry-border dispatch `FUN_004409f0` (with a
  confidence key; the deeper `.sht` format is a flagged sketch).

## Layout
| Path | What |
|---|---|
| `PCBdecomp.c` | Ghidra decompilation of th07.exe (~83k lines, ~94% of functions). The RE ground truth. **Partial** — some top-level loop fns (frame governor) are absent; see the Fork-A doc. |
| `src/netplay/` | Engine-agnostic netcode core (`netcode.*`, `Connection.*`, `merge.*` lifted from th06_multi_net), the C-linkage shim for coop.c (`netcode_c_api.*`), and the th07 integration DLL (`coop_net.cpp`). |
| `src/coop/coop.c` | Fork B — the second-player (P2) entity graft (clone, killable, grazes, separate bombs/power, shared border, ghost mode + F11 revive). |
| `src/harness/harness.c` | Determinism harness (record/replay the RNG seed+counter to prove the sim is deterministic). |
| `src/injector/injector.c` | Launches th07.exe suspended + LoadLibrary-injects a DLL. |
| `tests/` | `merge_test` (native, no wine — exhaustive merge-agreement check, runs in CI), `netloop_test` (in-process transport+merge), and `netsim` + `run_netsim.sh` (two-process lockstep integration test). |
| `third_party/minhook/` | MinHook (function detours). |
| `reference/th06_multi_net/` | The reference mod's source (the "phrasebook"). |

## Build & test

**On Windows** (ships the actual play artifacts): `powershell -File build.ps1`
(needs llvm-mingw i686 — see the script header).

**On Linux** (full build + automated tests, no game needed):
```sh
sudo apt-get install -y mingw-w64
# optional, to also RUN the Windows-side tests:
sudo dpkg --add-architecture i386 && sudo apt-get update
sudo apt-get install -y --no-install-recommends wine wine32:i386

./build.sh --test      # builds everything, runs the native merge test
                       # (+ the wine netcode tests when wine is present)
```
`build.sh` produces `th07_harness.dll`, `th07_coop.dll`, `th07_coop_net.dll`,
`injector.exe`, and the test exes in `build/`, and asserts every PE is 32-bit.
The netcode lockstep core is verified end-to-end by `tests/run_netsim.sh`
(a real host+guest over UDP loopback).

**CI:** `.github/workflows/build.yml` runs `./build.sh --clean --test` (Linux build of
all artifacts + the native merge test) on every push/PR.

## Run (in-game)
```
injector.exe "D:\Touhou 7 - Perfect Cherry Blossom\th07.exe"
```
(or use `build/run_coop.bat`). The injector loads whichever DLL is configured —
harness uses `harness.ini`, the netcode integration DLL uses `coop_net.ini`. For the
co-op DLL: get into a stage, P2 auto-spawns after ~3 s; see the header comment in
`src/coop/coop.c` for the live hotkeys (IJKL/Space/U/O = P2, F2–F12 = toggles).

## Status (2026-06-15)
- 🟡 **Proximity transparency prototype (§5l, NIGHT_SHIFT #2)**: fade the OTHER
  player out as the two overlap so your own stays clear. Asymmetric/per-instance
  (host fades P2, guest fades P1; single-machine fades P2 near P1). Behind
  `coop.ini [coop] proximity_fade=1` (default off). Compile-verified; needs a
  visual look to tune the ramp + a netplay test for the real asymmetry.
- 🟡 **Netplay wired into the co-op DLL (§5k, fork A §8)**: the netcode core now
  links into `th07_coop.dll`; P2's input comes from the WIRE (merged high bits)
  instead of the local keyboard, the menu/scene seam (`FUN_00437c70`) locksteps
  input + the seed-sync seam (`FUN_00442c60`) forces a shared RNG seed. Gated
  behind `coop.ini [net] enabled=1` (default OFF → unchanged local-keyboard
  baseline). Compile + native-merge-test verified; **needs a two-machine network
  test** (checklist in handoff §5k). Known limitation: under netplay the two-pass
  per-player char-select is bypassed (menus navigate together, P2 clones P1's
  char) — per-player char-over-wire is the follow-up (§8e).

## Status (2026-06-14)
- ✅ **EoSD-style menu character select**: P1 picks, then P2 picks its own char+type,
  then the game starts (handoff §5g). Different-char P2 works (body/shots/bomb/lasers).
- 🟡 **P2 icon HUD (§8a, PR #2)**: lives/bombs as P1-style `front.anm` icon sprites +
  power number, drawn in ZUN's sidebar pass below the point-item line; F12 toggles to
  the legacy text line. **Awaiting in-game visual confirm.**
- 🟡 **"P2 SELECT" menu prompt (§8d, PR #2)**: on-screen cue during P2's pick. Position
  tunable; awaiting visual confirm.
- ✅ RE coverage extended (2026-06-14): HUD/sprite, enemy-bullet, and enemy systems now
  mapped (`docs/th07_{hud_sprite,bullet,enemy}_system.md`); player shot/bomb offsets in
  `th07_player_struct.md`. The ECL danmaku VM (`FUN_00410520`) is the lone function
  Ghidra can't decompile in this dump — map it from another source.

## Status (2026-06-12)
- ✅ Netcode core: built + unit-tested + **lockstep verified end-to-end** (`netsim`),
  with a CI-run native merge test and a C-callable shim (`netcode_c_api`) ready for
  the coop.c integration.
- ✅ Determinism harness + injector: built, smoke-tested in-game.
- ✅ Fork B (P2 entity): substantial — spawns, killable by bullets/lasers/contact,
  **grazes** (the graze flag gates bullet hit tests — found 2026-06-12), collects
  items into its OWN power/bombs/lives, shared team border (P2 rides P1's border as
  a ringless shadow), ghost mode. All game-tested.
- 🟡 EoSD-style resurrection + life sharing, now **symmetric** (either player
  ghosts; the partner revives). A **phantom spare** keeps the engine from ever
  seeing a 0-lives death, so last-life deaths take the normal path (partial
  power drop — no full-power items, no continue-reset cheese) and **game over
  only happens when both players are down at once**. Revive = graze the ghost
  24px/90 frames then release focus → donate a life (free if broke); revives
  with 0 spares + death-stock bombs + post-drop power; guaranteed 1up at the
  death spot. **Life sharing**: two live players graze 90 frames without
  shooting, donor releases focus → drops a 1up for the partner. Graze feedback
  is visual/SFX-only. **Awaiting test.** F11 = debug instant-revive.
- ✅ Tier-1 boss/enemy HP scaling: **damage-side divisor** (`FUN_0043d9e0`
  return ÷ player count) — game-tested (Cirno ~2× TTK). Known shelved
  trade-off: popcorn takes 2 homing amulets (`docs/th07_boss_hp_scaling.md`).
- 🟡 Fork A (netcode↔game wiring): integration DLL written + compile-verified, **not
  yet game-tested** (deferred until the game side is finished). Seams pinned in
  `docs/th07_fork_a_integration.md`, including the A1 menu-lockstep hook.
- ⬜ P2 HUD (lives/bombs/power — cherry stays a single shared counter, per user),
  netcode→coop.c wiring, real seed handshake (ConnectionUI port), 3-player. See
  the handoff §5 next-steps.
