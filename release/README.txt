==========================================================================
  Touhou 7 — Perfect Cherry Blossom : CO-OP MOD  (beta)
  Local 2-player on one PC, or 2-player over the network.
==========================================================================

WHAT THIS IS
  A mod that adds a second player to PCB. Play on one keyboard, or hook up
  with a friend over the internet / LAN. It runs by injecting into the game
  at launch — your th07.exe is never modified.


----------------------------------------------------------------------------
WHAT YOU NEED
----------------------------------------------------------------------------
  * Touhou 7 - Perfect Cherry Blossom, version 1.00b (the English-patched or
    original 1.00b — NOT 1.00a or 1.00). Both players must use 1.00b online.
  * These two files, dropped INTO your PCB game folder (the folder that has
    th07.exe):
        launcher.exe
        th07_coop.dll
  * That's it. Run launcher.exe (NOT th07.exe directly).


----------------------------------------------------------------------------
LOCAL CO-OP (one PC, two players on one keyboard)
----------------------------------------------------------------------------
  1. Run  launcher.exe
  2. Click  [ Local Co-op ]
  3. The game starts. Pick P1's character as usual.
  4. About 3 seconds into the stage, Player 2 spawns automatically.

  Player 1 : the normal PCB controls (arrows / Z / X / Shift).
  Player 2 : I J K L = move,  Space = shoot,  U = focus,  O = bomb.


----------------------------------------------------------------------------
ONLINE CO-OP (two PCs)
----------------------------------------------------------------------------
  One player is the HOST, the other CONNECTS. Decide who hosts.

  HOST:
    1. Find your IP. On a LAN, open Command Prompt and run  ipconfig  — use
       the IPv4 Address (192.168.x.x). Over the internet, use your public IP
       and forward the Port (default 47000, UDP) to your PC.
    2. Run launcher.exe. Leave Port at 47000 (or pick your own). Set the
       input delay (2 is a good start; raise it if the game feels jittery).
    3. Click  [ Host Game ]. Sit at the title screen.
    4. Tell the other player your IP and Port.

  GUEST (Connect):
    1. Run launcher.exe.
    2. Type the host's IP into "Host IP", and the SAME Port the host used.
    3. Click  [ Connect ]. Sit at the title screen.

  Both machines auto-link while sitting at the title. Once linked you'll see
  a "NET" status line in the top-right during play, and coop_log.txt will
  read "LINK UP". Then:
    * The HOST drives the menus (difficulty, etc.).
    * Character select is PER-PLAYER: the host picks P1's character, then the
      screen says "P2 SELECT" and the guest picks their own. You can be
      different characters.
    * In the stage, each of you controls your own ship over the net.

  Tip: the host's input-delay and RNG seed are sent to the guest
  automatically — the guest doesn't need to match them.


----------------------------------------------------------------------------
THE "NET" STATUS LINE  (top-right, during play)
----------------------------------------------------------------------------
  NET H F1234 d2 SYNC   = you are Host, logic frame 1234, delay 2, in sync.
  NET G ... DSYN        = a desync was detected this frame (see beta notes).
  NET ... WAIT350ms     = the game is waiting on the other PC's input — if
                          this keeps climbing, their PC fell behind or the
                          connection hiccuped.
  NET LOST              = the link dropped; you're back to local P2.

  A full timeline is written to  coop_log.txt  next to th07_coop.dll. If you
  hit a bug, that file (from BOTH PCs) is the bug report.


----------------------------------------------------------------------------
TROUBLESHOOTING
----------------------------------------------------------------------------
  "DLL failed to load / wrong version"   You're not on th07.exe 1.00b.
  Stuck, never links                     Guest's Host IP/Port wrong, host's
                                         firewall blocking UDP, or (internet)
                                         the port isn't forwarded.
  "VERSION MISMATCH" in the log          The two PCs have different
                                         th07_coop.dll builds. Use the same.
  Game won't start at all                Run launcher.exe, not th07.exe.

  Windows may pop a firewall prompt the first time — click Allow (Private).


----------------------------------------------------------------------------
BETA NOTES  (please read)
----------------------------------------------------------------------------
  Local co-op is solid. ONLINE play is still experimental: long sessions can
  desync (the two screens drift apart). The NET status line and coop_log.txt
  are there so we can pin down exactly when/where — if it happens, grab both
  logs. Local-keyboard co-op is unaffected.

  Have fun, and report anything weird.
==========================================================================
