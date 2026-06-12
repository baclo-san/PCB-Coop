// merge_test.cpp — NATIVE (platform-independent) unit test for MergeKeys().
//
// Unlike netloop_test.cpp (which exercises the real Winsock transport and so only
// builds/runs under mingw + Windows/wine), this test depends ONLY on merge.cpp +
// merge.hpp, so it compiles and runs with a plain host g++/clang on Linux CI:
//
//     g++ -std=c++17 tests/merge_test.cpp src/netplay/merge.cpp -o merge_test && ./merge_test
//
// It locks down the two invariants the whole lockstep design rests on:
//   (1) host and guest, given mirrored (self,rcv) words, compute the IDENTICAL
//       merged word — exhaustively over all 2^16 host inputs x a P2 sample set;
//   (2) the bit mapping: P1 -> low 9 bits, P2 -> high bits, with the documented
//       per-button correspondence, and no cross-leak between the two.
#include "../src/netplay/merge.hpp"
#include <cstdio>

static int g_fails = 0;
#define CHECK(cond, msg) do { \
        if (cond) { printf("  [PASS] %s\n", msg); } \
        else      { printf("  [FAIL] %s\n", msg); g_fails++; } \
    } while (0)

// The low-bit -> P2-high-bit correspondence MergeKeys must honour.
struct Pair { unsigned short lo, hi; const char* name; };
static const Pair kP2Map[] = {
    { NB_SHOOT, NB_SHOOT2, "SHOOT" },
    { NB_BOMB,  NB_BOMB2,  "BOMB"  },
    { NB_FOCUS, NB_FOCUS2, "FOCUS" },
    { NB_UP,    NB_UP2,    "UP"    },
    { NB_DOWN,  NB_DOWN2,  "DOWN"  },
    { NB_LEFT,  NB_LEFT2,  "LEFT"  },
    { NB_RIGHT, NB_RIGHT2, "RIGHT" },
};
static const int kNP2 = (int)(sizeof(kP2Map) / sizeof(kP2Map[0]));

// ---- 1. host==guest agreement, exhaustive over host inputs ------------------
static void TestAgreementExhaustive()
{
    printf("[merge] host==guest for ALL host inputs x P2 sample set\n");

    // P2 (guest gameplay) only ever presses the 9 low gameplay bits; sweep a
    // representative set including every single bit and a few combos.
    unsigned short p2samples[] = {
        0x0000, 0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040,
        0x0080, 0x0100, 0x0055, 0x00AA, 0x00FF, 0x0108, 0x004F, 0x01FF,
    };
    const int nS = (int)(sizeof(p2samples) / sizeof(p2samples[0]));

    bool allAgree = true;
    long mismatches = 0;
    for (int gi = 0; gi < nS; gi++) {
        unsigned short guestIn = p2samples[gi];
        for (unsigned hostIn = 0; hostIn <= 0xFFFF; hostIn++) {
            unsigned short h = MergeKeys(true,  false, (unsigned short)hostIn, guestIn);
            unsigned short g = MergeKeys(false, false, guestIn, (unsigned short)hostIn);
            if (h != g) { allAgree = false; mismatches++; }
        }
    }
    if (!allAgree) printf("    (%ld mismatching (host,guest) words)\n", mismatches);
    CHECK(allAgree, "host and guest compute the identical word for every sampled combo");
}

// ---- 2. bit mapping + no cross-leak -----------------------------------------
static void TestBitMapping()
{
    printf("[merge] P1->low, P2->high, per-button mapping, no leak\n");

    for (int i = 0; i < kNP2; i++) {
        // host presses ONLY this gameplay button; guest presses ONLY this button.
        unsigned short hostMerged = MergeKeys(true, false, kP2Map[i].lo, kP2Map[i].lo);

        // P1 keeps it in the low bit
        CHECK((hostMerged & kP2Map[i].lo) != 0,
              kP2Map[i].name);   // label printed via name; low bit present
        // P2's same button lands in the high bit
        CHECK((hostMerged & kP2Map[i].hi) != 0,
              "  ^ also mapped into the P2 high bit");
        // guest's press must NOT additionally set any OTHER low gameplay bit
        unsigned short otherLow = (unsigned short)((hostMerged & 0x01FF) & ~kP2Map[i].lo);
        CHECK(otherLow == 0, "  ^ guest press did not leak into other P1 low bits");
    }

    // MENU and SKIP are shared (not split into a P2 variant): a guest press lands
    // back in the same low bit, never in any high bit.
    unsigned short m = MergeKeys(true, false, 0, NB_MENU);
    CHECK((m & NB_MENU) != 0 && (m & 0xFE00) == 0, "MENU is shared (no high-bit variant)");
    unsigned short s = MergeKeys(true, false, 0, NB_SKIP);
    CHECK((s & NB_SKIP) != 0 && (s & 0xFE00) == 0, "SKIP is shared (no high-bit variant)");
}

// ---- 3. UI mode merges both sides into the low bits -------------------------
static void TestUIMode()
{
    printf("[merge] UI mode ORs both players' words (menu navigation together)\n");
    CHECK(MergeKeys(true,  true, NB_SHOOT, NB_MENU)  == (unsigned short)(NB_SHOOT | NB_MENU),
          "host UI mode = self | rcv");
    CHECK(MergeKeys(false, true, NB_MENU,  NB_SHOOT) == (unsigned short)(NB_SHOOT | NB_MENU),
          "guest UI mode = self | rcv (same result)");
    // UI mode must be host/guest-symmetric for identical (self|rcv) inputs.
    bool sym = true;
    for (unsigned a = 0; a < 0x200; a++)
        for (unsigned b = 0; b < 0x200; b++)
            if (MergeKeys(true, true, (unsigned short)a, (unsigned short)b) !=
                MergeKeys(false, true, (unsigned short)b, (unsigned short)a))
                sym = false;
    CHECK(sym, "UI merge is host/guest symmetric over the low-9-bit space");
}

int main()
{
    printf("=== PCB co-op MergeKeys native unit test ===\n");
    TestAgreementExhaustive();
    TestBitMapping();
    TestUIMode();
    printf("=== %s (%d failure%s) ===\n",
           g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails, g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
