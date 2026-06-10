// netloop_test.cpp — self-test for the PCB co-op netcode core.
//
// 1. Transport: real UDP loopback round-trip of a Pack (Guest->Host->Guest),
//    verifying CtrlPack contents survive the wire.
// 2. Merge:    MergeKeys() produces the SAME 16-bit word on both machines, with
//    P1 in the low bits (host's input) and P2 in the high bits (guest's input).
//
// Exit code 0 = all pass, nonzero = failures. No game required.
#include "../src/netplay/Connection.hpp"
#include "../src/netplay/netcode.hpp"
#include "../src/netplay/netcode_internal.hpp"
#include <cstdio>

static int g_fails = 0;
#define CHECK(cond, msg) do { \
        if (cond) { printf("  [PASS] %s\n", msg); } \
        else      { printf("  [FAIL] %s\n", msg); g_fails++; } \
    } while (0)

// ---- 1. transport round-trip ------------------------------------------------
static bool PollWithRetry(Host& h, Pack& out)
{
    for (int i = 0; i < 1000; i++) {        // up to ~1s
        bool hasData = false;
        h.PollReceive(out, hasData);
        if (hasData) return true;
        Sleep(1);
    }
    return false;
}
static bool PollWithRetry(Guest& g, Pack& out)
{
    for (int i = 0; i < 1000; i++) {
        bool hasData = false;
        g.PollReceive(out, hasData);
        if (hasData) return true;
        Sleep(1);
    }
    return false;
}

static void TestTransport()
{
    printf("[transport] UDP loopback round-trip\n");
    Host  host;
    Guest guest;
    const int hostPort = 47100, guestPort = 47101;

    if (!host.Start("", hostPort, AF_INET))  { printf("  [FAIL] host.Start\n");  g_fails++; return; }
    if (!guest.Start("127.0.0.1", hostPort, guestPort, AF_INET)) {
        printf("  [FAIL] guest.Start\n"); g_fails++; return;
    }

    // guest -> host
    Pack tx;
    tx.type = 4;
    tx.ctrl.ctrl_type = Ctrl_Key;
    tx.ctrl.frame = 12345;
    for (int i = 0; i < KeyPackFrameNum; i++) {
        ReadFromInt(tx.ctrl.keys[i], (unsigned short)(0x1000 + i));
        tx.ctrl.rng_seed[i] = (unsigned short)(0xABC0 + i);
        tx.ctrl.igc_type[i] = IGC_NONE;
    }
    CHECK(guest.SendPack(tx), "guest.SendPack");

    Pack rx;
    bool got = PollWithRetry(host, rx);
    CHECK(got, "host received the packet");
    if (got) {
        CHECK(rx.ctrl.frame == 12345, "frame survived the wire");
        unsigned short k0 = 0; WriteToInt(rx.ctrl.keys[0], k0);
        CHECK(k0 == 0x1000, "keys[0] survived");
        CHECK(rx.ctrl.rng_seed[7] == (unsigned short)(0xABC0 + 7), "rng_seed[7] survived");
        CHECK(!host.GetGuestIp().empty(), "host learned guest address from recvfrom");
    }

    // host -> guest (reply path)
    Pack reply;
    reply.type = 4;
    reply.ctrl.ctrl_type = Ctrl_Key;
    reply.ctrl.frame = 999;
    CHECK(host.SendPack(reply), "host.SendPack (reply to learned guest)");
    Pack rg;
    bool got2 = PollWithRetry(guest, rg);
    CHECK(got2 && rg.ctrl.frame == 999, "guest received the reply");
}

// ---- 2. merge correctness ---------------------------------------------------
static void TestMerge()
{
    printf("[merge] both machines agree on one word\n");

    // P1 (host) holds: SHOOT + LEFT.  P2 (guest) holds: FOCUS + RIGHT.
    unsigned short hostIn  = NB_SHOOT | NB_LEFT;
    unsigned short guestIn = NB_FOCUS | NB_RIGHT;

    // host computes with self=host, rcv=guest; guest computes self=guest, rcv=host.
    unsigned short hostMerged  = MergeKeys(true,  false, hostIn,  guestIn);
    unsigned short guestMerged = MergeKeys(false, false, guestIn, hostIn);

    CHECK(hostMerged == guestMerged, "host and guest compute the identical word");

    // P1 low bits == host's input
    CHECK((hostMerged & (NB_SHOOT | NB_LEFT)) == (unsigned short)(NB_SHOOT | NB_LEFT),
          "P1 low bits carry host's SHOOT+LEFT");
    // P2 high bits == guest's input mapped up
    CHECK((hostMerged & NB_FOCUS2) != 0, "P2 high bits carry guest's FOCUS (as FOCUS2)");
    CHECK((hostMerged & NB_RIGHT2) != 0, "P2 high bits carry guest's RIGHT (as RIGHT2)");
    // guest's low-bit FOCUS/RIGHT must NOT leak into P1
    CHECK((hostMerged & NB_FOCUS) == 0, "guest FOCUS did not leak into P1 low bits");
    CHECK((hostMerged & NB_RIGHT) == 0, "guest RIGHT did not leak into P1 low bits");

    // UI mode merges both into low bits (menu navigation together)
    unsigned short ui = MergeKeys(true, true, NB_SHOOT, NB_MENU);
    CHECK(ui == (unsigned short)(NB_SHOOT | NB_MENU), "UI mode ORs both players' low bits");

    // exhaustive agreement over a bunch of combinations
    bool allAgree = true;
    unsigned short samples[] = { 0x0000, 0x0001, 0x00FF, 0x0055, 0x00AA, 0x0108, 0x004F };
    for (unsigned a = 0; a < sizeof(samples)/sizeof(samples[0]); a++)
        for (unsigned b = 0; b < sizeof(samples)/sizeof(samples[0]); b++)
        {
            unsigned short h = MergeKeys(true,  false, samples[a], samples[b]);
            unsigned short g = MergeKeys(false, false, samples[b], samples[a]);
            if (h != g) allAgree = false;
        }
    CHECK(allAgree, "host==guest for all sampled input combinations");
}

int main()
{
    printf("=== PCB co-op netcode self-test ===\n");

    // Hold our own Winsock reference for the whole test. ConnectionBase refcounts
    // WSAStartup/WSACleanup with a shared static, and Start() does Reset()->
    // CleanupWinsock() first; creating a Host and Guest in ONE process would
    // otherwise let that refcount hit 0 and WSACleanup() the host's live socket.
    // (Real usage is one peer per process, so this only matters for the test.)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    TestTransport();
    TestMerge();

    WSACleanup();
    printf("=== %s (%d failure%s) ===\n",
           g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails, g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
