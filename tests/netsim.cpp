// netsim.cpp — two-process integration driver for the PCB co-op lockstep core.
//
// The in-process self-test (netloop_test) only covers transport + the pure
// MergeKeys(). It never runs the actual per-frame lockstep — the delay buffer,
// the real UDP frame exchange, and the RNG-seed sync detector inside
// Netcode_GetInput_Net(). netcode.cpp keeps GLOBAL state (one peer per
// process, exactly as the th06 reference), so a true two-peer test needs two
// processes. This driver is one of them; tests/run_netsim.sh launches a host
// and a guest under wine, then diffs their per-frame output.
//
//   netsim.exe <host|guest> <out.csv> [frames] [delay] [desyncAt]
//
// Each side feeds a DETERMINISTIC, role-specific scripted local input and a
// scripted RNG seed into the netcode, runs `frames` logic frames, and writes
// "frame,word,sync" per line. The lockstep guarantee being verified:
//   * word[f] is IDENTICAL on host and guest for every frame (the whole point:
//     both machines compute the same merged input word).
//   * sync==1 while both sides report the same seed; if `desyncAt` is given the
//     guest perturbs its seed from that frame, and BOTH sides must flip sync->0
//     `delay` frames later (the seed-mismatch oracle fires) while words stay
//     identical (input merge is independent of the seed check).
//
// Exit code is always 0 here; the PASS/FAIL verdict is the diff in run_netsim.sh.
#include "../src/netplay/netcode.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static int g_role      = 0;    // 0 = host, 1 = guest
static int g_curFrame  = 0;
static int g_desyncAt  = -1;

// Role-specific scripted input so the merge is non-trivial and the P1/P2 split
// is observable: host holds SHOOT (+LEFT on even frames); guest holds FOCUS
// (+RIGHT on odd frames). Both sides run the SAME script for BOTH roles, so the
// merged word is reproducible on each machine.
static unsigned short ScriptInput(int role, int frame)
{
    unsigned short w = 0;
    if (role == 0) { w = NB_SHOOT; if ((frame & 1) == 0) w |= NB_LEFT;  }
    else           { w = NB_FOCUS; if ((frame & 1) == 1) w |= NB_RIGHT; }
    return w;
}

static unsigned short cbInput(void)
{
    return ScriptInput(g_role, g_curFrame);
}

// Deterministic per-frame seed, identical formula on both sides → sync holds.
// When desyncAt is set, the guest perturbs its seed from that frame onward so
// the seed-mismatch detector has something to catch.
static unsigned short cbSeed(void)
{
    unsigned int h = (unsigned int)g_curFrame * 2654435761u + 0x12345u;
    unsigned short s = (unsigned short)(h ^ (h >> 16));
    if (g_desyncAt >= 0 && g_curFrame >= g_desyncAt && g_role == 1)
        s ^= 0xBEEF;
    return s;
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <host|guest> <out.csv> [frames] [delay] [desyncAt]\n", argv[0]);
        return 2;
    }
    g_role = (strcmp(argv[1], "host") == 0) ? 0 : 1;
    const char* outPath = argv[2];
    int frames   = (argc > 3) ? atoi(argv[3]) : 300;
    int delay    = (argc > 4) ? atoi(argv[4]) : 2;
    g_desyncAt   = (argc > 5) ? atoi(argv[5]) : -1;

    NetcodeCallbacks cb = { cbInput, cbSeed };
    Netcode_SetCallbacks(cb);

    const int port = 47200;
    bool ok = g_role == 0
        ? Netcode_StartHost("", port, AF_INET)
        : Netcode_StartGuest("127.0.0.1", port, port + 1, AF_INET);
    if (!ok) { fprintf(stderr, "[%s] transport start failed\n", argv[1]); return 3; }

    // Both sides agree on delay + a shared start seed (the handshake's job in the
    // real game; hard-coded equal here).
    Netcode_SetConnected(true, delay, 0x1234);

    FILE* out = fopen(outPath, "w");
    if (!out) { fprintf(stderr, "[%s] cannot open %s\n", argv[1], outPath); return 4; }
    fprintf(out, "frame,word,sync\n");

    for (int f = 0; f < frames; f++) {
        g_curFrame = f;
        int ctrl = 0;
        unsigned short word = Netcode_GetInput_Net(f, /*is_in_UI=*/false, ctrl);
        fprintf(out, "%d,%u,%d\n", f, (unsigned)word, Netcode_IsSync() ? 1 : 0);
        if (!Netcode_IsConnected()) {
            fprintf(out, "# disconnected at frame %d\n", f);
            break;
        }
    }

    fclose(out);
    fprintf(stderr, "[%s] done: %d frames -> %s\n", argv[1], frames, outPath);
    return 0;
}
