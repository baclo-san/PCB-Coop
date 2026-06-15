# Two-player netplay test — quick guide

First real over-the-network test of the co-op mod. **Two PCs, two people.** One is
the **host**, the other the **guest**. Plan for ~10 min of setup.

> Status: netcode is unit- + lockstep-tested but never run machine-to-machine.
> Expect to find bugs — that's the point. Capture `coop_log.txt` from both sides.

## 0. Both machines need (must match!)
- **The same `th07.exe`, version 1.00b.** The mod is hard-coded to that exact build
  (SHA256 `35467EAF…E80CA`). A different version will crash or misbehave.
- The `build/` folder from this repo: **`injector.exe`, `th07_coop.dll`, `coop.ini`**
  (keep the three together in one folder).
- Be on the **same network** (same Wi-Fi/LAN) for the first test — far simpler than
  going over the internet.

## 1. Find the host's IP
On the **host** PC, open Command Prompt and run `ipconfig`. Note the **IPv4 Address**
(looks like `192.168.x.x`). The guest needs this.

## 2. Edit `coop.ini` (next to the DLL)

**Host** machine:
```ini
[net]
enabled = 1
role  = host
port  = 47000
delay = 2
seed  = 0x1234
```

**Guest** machine (set `peer` to the host's IPv4 from step 1):
```ini
[net]
enabled = 1
role  = guest
peer  = 192.168.x.x      ; <-- the HOST's IP
port  = 47000
local = 47001
```
The link **auto-connects** (a handshake), and the **host pushes its `delay` + `seed`
to the guest** — so on the guest those two are ignored (you can omit them). Only the
host's `delay`/`seed` matter. No more hand-matching.

## 3. Allow the host through the firewall
On the host, Windows Firewall must let **inbound UDP on port 47000** through. Easiest:
the first time `th07.exe` networks, Windows pops a "allow access?" dialog — click
**Allow** (Private networks). If you miss it, add an inbound UDP 47000 rule manually.
(Guest needs no inbound rule.)

## 4. Launch (either order — it auto-connects)
Use the injector (NOT a normal th07 launcher, or the mod won't load). Edit
`run_coop.bat` so its path points at your `th07.exe`, then double-click it — or run:
```
injector.exe "C:\path\to\th07.exe" "th07_coop.dll"
```
You should see `injected OK`. Launch both machines and leave them at the **title
screen**; the handshake keeps retrying until the peer appears, so launch order
doesn't matter. (Sit at the title until connected — see step 5.)

## 5. Confirm the link
Open **`coop_log.txt`** (created next to the DLL) on each machine. At first you'll see
`transport up ... Handshaking — waiting for the peer`. Once they find each other:
```
netplay: LINK UP (handshake done). role=host delay=2 seed=0x1234. ...
```
Both machines should log `LINK UP` with the **same seed** (the host's). If you only
see `Handshaking...` forever, the peer isn't reachable — see Troubleshooting.

## 6. Play
- **Title / difficulty:** the **host (P1)** leads — host navigates and starts.
  (Both can nudge the title cursor; let the host drive to avoid fighting.)
- **Character select (per-player!):** the host picks **P1's** character + shot type
  and confirms. The screen then shows **"P2 SELECT CHARACTER"** — now the **guest
  (P2)** picks their OWN character + shot with their controls and confirms. The game
  starts once both have chosen, each as their own character (they can differ!).
- In the stage, each person controls their own ship over the network. Watch for:
  desync (the two screens drift apart), input lag (raise/lower `delay`), or stalls.

## Troubleshooting
| Symptom | Likely cause / fix |
|---|---|
| `transport start FAILED` | Port in use or firewall. Try another `port` on both; allow UDP. |
| Stuck on `Handshaking...` | Peer unreachable: wrong `peer` IP, different network, or host firewall blocking UDP 47000. |
| `peer VERSION MISMATCH` | The two machines have different `th07_coop.dll` builds. Use the same DLL on both. |
| `DESYNC detected` (in-game) | Seed is auto-synced now, so this means a logic divergence — grab both `coop_log.txt` and report. |
| Choppy / laggy | Raise the HOST's `delay` (e.g. 3–4) — trades input latency for smoothness (pushed to the guest). |
| Mod not loaded at all | You launched th07 normally. Must go through `injector.exe` / `run_coop.bat`. |
| One side crashes | Grab BOTH `coop_log.txt` files + note what was on screen; that's the bug report. |

## Notes / known limits (this cut)
- **Auto-connect handshake**: launch order doesn't matter; the host's `delay`+`seed`
  are pushed to the guest (guest's are ignored).
- **Per-player character select works** (host=P1 then guest=P2). Title/difficulty
  are host-led.
- Local-keyboard co-op is unchanged — set `enabled = 0` to get it back.
