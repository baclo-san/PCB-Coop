# Two-player netplay test — quick guide

First real over-the-network test of the co-op mod. **Two PCs, two people.** One is
the **host**, the other **connects**. Plan for ~10 min of setup.

> Status: links and plays, but long online sessions can still desync (see
> **Known issue** at the bottom). Capture `coop_log.txt` from BOTH sides — with the
> new sync telemetry it now carries a frame-by-frame timeline of any divergence.

## 0. Both machines need (must match!)
- **The same `th07.exe`, version 1.00b.** The mod is hard-coded to that exact build
  (SHA256 `35467EAF…E80CA`). A different version will refuse to load.
- **`launcher.exe` + `th07_coop.dll`** dropped into the PCB game folder (next to
  `th07.exe`). That's the whole beta drop (`build/release/PCB-Coop-beta.zip`).
- Same network (LAN) for the first test — far simpler than going over the internet.

## 1. Find the host's IP
On the **host** PC: `ipconfig` → note the **IPv4 Address** (`192.168.x.x`). For an
internet test, use the public IP and forward the **Port (UDP, default 47000)**.

## 2. Launch with `launcher.exe` (NOT th07.exe directly)
Run `launcher.exe` on both PCs. It finds `th07.exe` in its own folder (Browse… to
override) and writes `coop.ini` for you — no hand-editing.

- **Host:** leave Port at `47000` (or pick one), set input delay (2 to start),
  click **Host Game**.
- **Guest:** type the host's IP in **Host IP**, the **same Port**, click **Connect**.

The host's delay + a freshly-minted RNG seed are pushed to the guest automatically,
so the guest doesn't match anything by hand.

For a single-PC sanity check, click **Local Co-op** (P2 = IJKL/Space/U/O keyboard).

## 3. Firewall
The host must allow **inbound UDP on the Port**. The first time, Windows pops an
"allow access?" dialog — click **Allow** (Private). Guest needs no inbound rule.

## 4. Confirm the link
Both sit at the **title screen**; the handshake retries until the peer appears
(launch order doesn't matter). `coop_log.txt` (next to the DLL) shows:
```
netplay: transport up role=host ... Handshaking — waiting for the peer ...
netplay: LINK UP (handshake done). role=host delay=2 seed=0x....
```
Both should log `LINK UP` with the **same seed**. Then a **NET status line** appears
top-right in-game — and now also **top-left on the menu** (see below), so you can watch
the front-end sync state before the game even starts.

## 5. Play
- **Title / difficulty:** the **host (P1)** drives.
- **Character select (per-player):** host picks P1's character + shot, confirms;
  the screen shows **"P2 SELECT"**, then the **guest (P2)** picks their own. They can
  differ. Game starts once both have chosen.
- In the stage, each person controls their own ship over the net.

## The NET status line + sync log (new)
Top-right during play, e.g. `NET H F1234 d2 SYNC`:
- `H`/`G` = host/guest, `F####` = logic frame, `d#` = delay.
- `SYNC` / `DSYN` = in sync / a seed mismatch was seen this frame.
- `WAIT###ms` = this frame blocked that long waiting for the peer's input. **A
  climbing WAIT with a near-frozen frame number IS a stall** (the other PC fell
  behind or hit a loading hitch).
- `NET LOST` = the link timed out; you dropped back to local P2.

**On the menu** the same line shows top-left, with the two RNG seeds appended:
`NET H F1234 d2 SYNC AB12/AB12`. The trailing `self/rcv` pair is the desync oracle — the
instant they diverge (`… DSYN AB12/CD34`) the front-end has desynced, which is the usual
precursor to a menu→stage stall. Watch it while navigating: if `Esc+R` / returning to title
is what diverges them, you'll see it here in real time.

`coop_log.txt` mirrors this: a heartbeat every ~2s (`F#### SYNC wait=…ms rng
self=… peer=…`), a one-line `STALL` warning when a frame blocks ≥250 ms, and a
single `DESYNC in stage …` with the mismatched RNG seeds. **This is the bug report.**

## Troubleshooting
| Symptom | Likely cause / fix |
|---|---|
| `transport start FAILED` | Port in use / firewall. Try another Port on both. |
| Stuck `Handshaking…` | Wrong Host IP/Port, different network, or host firewall/port-forward blocking UDP. |
| `peer VERSION MISMATCH` | Different `th07_coop.dll` builds. Use the same DLL on both. |
| `STALL` / `WAIT` climbing then `NET LOST` | One PC stalled (load hitch / slow frame) past the 5s lockstep timeout. |
| `DESYNC in stage` | Logic divergence — grab both `coop_log.txt` and report (see Known issue). |
| Mod not loaded | You launched th07 directly. Use `launcher.exe`. |

## Known issue — menu-start desync (under investigation)
The lockstep currently goes live the instant the transport connects, which happens
at a **different menu moment on each PC**. So "frame 0" is not the same game state on
both, and the front-end RNG diverges immediately. That alone is cosmetic (menus
aren't RNG-locked, and the stage seed is forced at stage start), but the bigger
symptom — a stall/slowdown at the menu→stage transition then `peer lost` — points at
the two machines never sharing a common start state. **The fix is a real start
barrier** (both machines begin lockstep frame 0 from the *same* screen, the way
EoSD's "Start Game" button works), rather than connecting mid-menu. The new sync
telemetry above is the instrumentation to confirm that before/after.

### Field-test log — 2026-06-17 (P1 `tovrof.zip` / P2 `7454cb.zip`)
Three back-to-back attempts, same two PCs:
1. **Total failure** — never synced; repeated `Esc+R` and return-to-menu didn't recover.
2. **No FPU guard** (`[net] fpu_guard=0`) — synced in **game 3** and stayed synced through
   everything afterward.
3. **FPU guard back ON** — synced after **one restart**; "good enough."

Read: the sync is **fragile at start** (needs 1+ restarts) but **stable once locked**, and the
FPU guard isn't clearly helping/hurting the *start* path. The fragile-start + easy-menu-desync
both point at the same root cause as the Known issue above — **no shared start barrier**, so the
two machines begin lockstep from different states and only converge by luck after a restart. The
menu RNG readout (added this cut) is to catch exactly when/where that divergence happens. **The
real fix remains the EoSD-style "Start Game" barrier**, not more FPU tweaking. Tracked, not yet
implemented — it's the next netplay work item.

### Root cause CONFIRMED from those logs + two fixes (2026-06-18)
Reading `tovrof`/`7454cb` frame-by-frame settled three things — **none of it is the x87 control
word** (every `control word pinned` line is `was 0x001f` on *both* peers, so the §8q precision
theory is ruled out for these builds):

1. **The menu "desync" is real and game-breaking — different difficulties.** A live run put one
   peer on Hard, the other on Phantasm. Traced to ZUN's menu **key-repeat** (hold-to-scroll):
   `FUN_00437c70` (PCBdecomp.c:22803-22816) derives the repeat counter/flag (`DAT_004b9e60` /
   `DAT_004b9e5c`) from the **local keyboard poll**, *before* our merged-word overwrite. The
   cursor's hold-scroll is gated on that flag (PCBdecomp.c:29077), so holding a direction advances
   the cursor a **machine-dependent** number of steps. Taps already sync (they edge-detect
   cur/prev, which carry the merged word); only *holds* diverged. **Fix:** `SyncMenuRepeat()` now
   recomputes the repeat counter/flag from the **merged** word each menu frame, so hold-scroll is
   deterministic on both peers.
2. **The menu RNG heartbeat (`ctr=0`) is a different, harmless signal** — the menu draws no RNG,
   so `self/peer` just differ by an idle seed value. The difficulty fork above is an *input*
   divergence, not RNG. Both now visible on the menu HUD.
3. **Esc+R never re-seeded.** `SEEDFORCE` only fired on first stage entry from the menu; the
   `frame counter reset -> FRESH start` retry path stays in the same scene, so a forked stage
   could only re-align via a full title round-trip — exactly "couldn't sync no matter how many
   Esc+R" in attempt 1. **Fix:** the fresh-start retry now re-arms the seed force, so the next
   `HookedGameStart` re-applies the shared init seed on both peers (the same realign the HARD
   DESYNC log already names, now reachable from a quick restart).

Still open: the **first** menu→stage transition can still fork from front-end load drift (each PC
burns a different number of loading ticks); the scene-reset re-zero handles most of it but the
EoSD-style explicit start barrier remains the proper long-term fix.

## Notes / known limits (this cut)
- **Launcher** replaces hand-editing `coop.ini`. Auto-connect; host pushes delay+seed.
- **Per-player character select** works (host=P1 then guest=P2). Title/difficulty host-led.
- **Menu hold-to-scroll** is now lockstep-deterministic (difficulty can't fork between peers).
- **Proximity fade** is ON by default (set `[coop] proximity_fade=0` to disable).
- Local-keyboard co-op is unchanged — click **Local Co-op** (or `[net] enabled=0`).
