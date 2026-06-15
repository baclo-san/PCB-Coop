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
delay = 2
seed  = 0x1234
```
`delay` and `seed` **must be identical on both sides.** (`seed` mismatch = instant
desync; the log will say so.)

## 3. Allow the host through the firewall
On the host, Windows Firewall must let **inbound UDP on port 47000** through. Easiest:
the first time `th07.exe` networks, Windows pops a "allow access?" dialog — click
**Allow** (Private networks). If you miss it, add an inbound UDP 47000 rule manually.
(Guest needs no inbound rule.)

## 4. Launch — host first, then guest
Use the injector (NOT a normal th07 launcher, or the mod won't load). Edit
`run_coop.bat` so its path points at your `th07.exe`, then double-click it — or run:
```
injector.exe "C:\path\to\th07.exe" "th07_coop.dll"
```
You should see `injected OK`. Launch the **host** first, then the **guest** within a
few seconds.

## 5. Confirm the link
Open **`coop_log.txt`** (created next to the DLL) on each machine. Near the top you
want:
```
netplay: UP role=host ... P2 input now from the WIRE ...
```
If instead you see `transport start FAILED` or `DESYNC detected`, see Troubleshooting.

## 6. Play
- From the title, **one person navigates the menu** (in this cut menu input is
  merged, so both control the cursor — don't fight over it). Pick a character and
  start. **P2 plays the same character as P1** for now (per-player char over the wire
  is the next milestone).
- In the stage, each person controls their own ship over the network. Watch for:
  desync (the two screens drift apart), input lag (raise/lower `delay`), or stalls.

## Troubleshooting
| Symptom | Likely cause / fix |
|---|---|
| `transport start FAILED` | Port in use or firewall. Try another `port` on both; allow UDP. |
| Guest can't connect | Wrong `peer` IP, different network, or host firewall blocking UDP 47000. |
| `DESYNC detected` | `seed` or `delay` differ between the two `coop.ini`s. Make them identical. |
| Choppy / laggy | Raise `delay` (e.g. 3–4) on both — trades input latency for smoothness. |
| Mod not loaded at all | You launched th07 normally. Must go through `injector.exe` / `run_coop.bat`. |
| One side crashes | Grab BOTH `coop_log.txt` files + note what was on screen; that's the bug report. |

## Notes / known limits (this cut)
- Menus navigate **together**; **P2 = P1's character** (A2 cut — handoff §8e).
- No automatic seed handshake yet: it relies on matching `seed=` in both inis.
- Local-keyboard co-op is unchanged — set `enabled = 0` to get it back.
