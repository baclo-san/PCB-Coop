# PCB-Coop

Co-op (2-player) netplay mod for **Touhou 7 — Perfect Cherry Blossom** (`th07.exe`),
modeled on the EoSD co-op mod [RUEEE/th06_multi_net](https://github.com/RUEEE/th06_multi_net).
PCB has no public decompilation, so the mod is delivered as **injected DLLs that
detour `th07.exe` by address** (via [MinHook](third_party/minhook)). Reverse
engineering is *functional* (behavior-correct), not bit-matching.

> ⚠️ **Build-specific.** All hard-coded addresses target **PCB ver 1.00b**,
> SHA256 `35467EAF8DC7FC85F024F16FB2037255F151CEFDA33CF4867BC9122AAA2E80CA`
> (650,752 bytes). They will be wrong for any other release — re-pin first.

## Components

| Path | What it is |
|---|---|
| `src/coop/coop.c`     | `th07_coop.dll` — the **second-player entity** graft (piggyback clone of P1: update/draw/collision, separate bombs+power, ghost mode, shot transfer). P2 is currently driven by **local keyboard** (IJKL/Space/U/O). Optional boss-HP scaling on F5. |
| `src/netplay/`        | The engine-agnostic **lockstep netcode** (UDP transport + delay buffer + merge + RNG-seed sync). Ported from th06_multi_net. Builds + self-tests, but **not yet wired** into the game DLL. |
| `src/harness/`        | `th07_harness.dll` — determinism harness (forces a fixed seed, records/replays input, logs the RNG counter `0x0049fe24` as a desync oracle). |
| `src/injector/`       | `injector.exe` — launches `th07.exe` suspended and injects a DLL. |
| `tests/`              | `netloop_test.cpp` (Windows: UDP loopback + merge) and `merge_test.cpp` (**native**, runs on Linux CI — exhaustive merge-agreement check). |
| `PCBdecomp.c`         | The Ghidra C export of the pinned binary (~83k lines) — the RE ground truth. |
| `reference/th06_multi_net/` | The EoSD mod's netcode, for porting reference. |

## Build

Outputs land in `build/`. Everything that enters `th07.exe`'s address space is 32-bit.

- **Windows** (llvm-mingw): `powershell -ExecutionPolicy Bypass -File build.ps1`
- **Linux / web session** (distro mingw-w64):
  ```sh
  apt-get install -y mingw-w64
  ./build.sh --test          # builds all artifacts + runs the native merge test
  ```

CI (`.github/workflows/build.yml`) runs the Linux build + native test on every push/PR.

## Run

```
injector.exe "D:\Touhou 7 - Perfect Cherry Blossom\th07.exe" th07_coop.dll
```
(or use `build/run_coop.bat`). Get into a stage; P2 auto-spawns after ~3s. See the
header comment in `src/coop/coop.c` for the live hotkeys.

## Documentation (read in this order)

1. **[docs/th07_integration_forkA.md](docs/th07_integration_forkA.md)** — current state,
   the four game seams (verified against `PCBdecomp.c` with line numbers), Fork A
   (seed sync + menu lockstep), and the plan to wire the netcode into the co-op DLL.
2. **[docs/th07_gameplay_seams.md](docs/th07_gameplay_seams.md)** — gameplay RE:
   boss-HP scaling, cherry/RNG determinism, resources & anti-tamper, the death FSM
   and resurrection seam.
3. **[docs/th07_netplay_handoff.md](docs/th07_netplay_handoff.md)** — the original,
   broad handoff (some parts superseded; see its banner).

## Status (2026-06-12)

- ✅ Second-player **entity** works locally (Fork B largely done).
- ✅ Netcode core builds + passes tests.
- ⏳ **Not yet integrated:** the netcode is not wired to the co-op DLL (P2 = keyboard).
  Gated on Fork A (menu lockstep + seed sync) and the host/join UI. See doc #1.
- ⏳ Open gameplay design choices (per-player vs shared cherry/border, P2 HUD,
  resurrection, boss-HP tuning) need a live two-player test.
