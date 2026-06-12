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
`src/coop/coop.c` for the live hotkeys (IJKL/Space/U/O = P2, F4–F11 = toggles).

## Status (2026-06-12)
- ✅ Netcode core: built + unit-tested + **lockstep verified end-to-end** (`netsim`),
  with a CI-run native merge test and a C-callable shim (`netcode_c_api`) ready for
  the coop.c integration.
- ✅ Determinism harness + injector: built, smoke-tested in-game.
- ✅ Fork B (P2 entity): substantial — spawns, killable by bullets/lasers/contact,
  **grazes** (the graze flag gates bullet hit tests — found 2026-06-12), collects
  items into its OWN power/bombs/lives, shared team border (P2 rides P1's border as
  a ringless shadow), ghost mode. All game-tested.
- 🟡 EoSD-style resurrection: implemented (ghost auto-wanders the bottom band;
  survivor graze-revives it in 90 frames at the cost of a donated life — free if
  broke; guaranteed 1up drop on last-life death), **not yet game-tested**. F11 =
  debug instant-revive.
- 🔴 Tier-1 boss/enemy HP scaling: implemented (ECL set-life detour) but the
  2026-06-12 midboss test showed **no effect on Cirno** (fairies do scale); a
  per-cap-write diagnostic is in — next run pinpoints it
  (`docs/th07_boss_hp_scaling.md`; an alternative damage-side lever is documented there).
- 🟡 Fork A (netcode↔game wiring): integration DLL written + compile-verified, **not
  yet game-tested** (deferred until the game side is finished). Seams pinned in
  `docs/th07_fork_a_integration.md`, including the A1 menu-lockstep hook.
- ⬜ P2 HUD (lives/bombs/power — cherry stays a single shared counter, per user),
  netcode→coop.c wiring, real seed handshake (ConnectionUI port), 3-player. See
  the handoff §5 next-steps.
