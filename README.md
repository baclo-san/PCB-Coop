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
  §0–§1 (addresses), then §5b (latest progress) and §5 (next steps).
- **`docs/th07_fork_a_integration.md`** — verified hook seams for wiring the netcode
  into the game (the input-inject + seed-sync detours).
- **`docs/th07_boss_hp_scaling.md`** — Tier-1 boss/enemy HP scaling (easiest gameplay
  change).
- **`docs/th07_cherry_determinism.md`** — the cherry↔item-drop RNG coupling and how to
  do per-player cherry without breaking lockstep.
- **`docs/th07_player_struct.md`** — verified PCB Player-object offset map (for the
  Fork-B second-player graft).

## Layout
| Path | What |
|---|---|
| `PCBdecomp.c` | Ghidra decompilation of th07.exe (~83k lines, ~94% of functions). The RE ground truth. **Partial** — some top-level loop fns (frame governor, GameUpdate) are absent; see the Fork-A doc. |
| `src/netplay/` | Engine-agnostic netcode core (`netcode.*`, `Connection.*`, lifted from th06_multi_net) + the th07 integration DLL (`coop_net.cpp`). |
| `src/coop/coop.c` | Fork B — the second-player (P2) entity graft (clone, killable, separate bombs/power, ghost mode). |
| `src/harness/harness.c` | Determinism harness (record/replay the RNG seed+counter to prove the sim is deterministic). |
| `src/injector/injector.c` | Launches th07.exe suspended + LoadLibrary-injects a DLL. |
| `tests/` | `netloop_test` (in-process transport+merge) and `netsim` + `run_netsim.sh` (two-process lockstep integration test). |
| `third_party/minhook/` | MinHook (function detours). |
| `reference/th06_multi_net/` | The reference mod's source (the "phrasebook"). |

## Build & test

**On Windows** (ships the actual play artifacts): `powershell -File build.ps1`
(needs llvm-mingw i686 — see the script header).

**On Linux** (full build + automated tests, no game needed):
```sh
sudo apt-get install -y gcc-mingw-w64-i686 g++-mingw-w64-i686
# to also RUN the tests:
sudo dpkg --add-architecture i386 && sudo apt-get update
sudo apt-get install -y --no-install-recommends wine wine32:i386

./build.sh --test      # builds everything, runs the netcode self-tests under wine
```
`build.sh` produces `th07_harness.dll`, `th07_coop.dll`, `th07_coop_net.dll`,
`injector.exe`, and the two test exes in `build/`. The netcode lockstep core is
verified end-to-end by `tests/run_netsim.sh` (a real host+guest over UDP loopback).

## Run (in-game)
Copy `build/<dll> + injector.exe + the .ini` together, then:
```
injector.exe "D:\Touhou 7 - Perfect Cherry Blossom\th07.exe"
```
The injector loads whichever DLL is configured. (Harness uses `harness.ini`; the netcode
integration DLL uses `coop_net.ini`.) See the docs for what each does and what is /
isn't game-tested yet.

## Status (2026-06-11)
- ✅ Netcode core: built + unit-tested + **lockstep verified end-to-end** (`netsim`).
- ✅ Determinism harness + injector: built, smoke-tested in-game.
- ✅ Fork B (P2 entity): substantial — spawns, killable, separate bombs/power, ghost
  mode, F11 revive-from-ghost (debug stand-in for proximity/graze resurrection).
- 🟡 Tier-1 boss/enemy HP scaling: implemented in `coop.c` (scales with player count,
  F5 toggle), **compile-verified, not yet game-tested** (`docs/th07_boss_hp_scaling.md`).
- 🟡 Fork A (netcode↔game wiring): integration DLL written + compile-verified, **not yet
  game-tested**. Seams are pinned (`docs/th07_fork_a_integration.md`).
- ⬜ Menu lockstep (A1), real seed handshake (needs ConnectionUI port), per-player
  cherry/lives, auto-resurrection trigger, 3-player. See the handoff §5 next-steps.
