// merge.hpp — pure input-merge logic for the PCB co-op netcode.
//
// This is the ONE place that defines the 16-bit button layout (P1 in the low 9
// bits, P2 in the high 7) and the host==guest merge invariant. It is engine- AND
// platform-independent: no winsock, no <windows.h>, no STL transport. The rest of
// the netcode (Connection/netcode.cpp) pulls in Winsock, which means it can only
// build under mingw/MSVC; splitting the merge out lets it be unit-tested NATIVELY
// (tests/merge_test.cpp, runs under plain g++/clang on Linux CI — no game, no wine).
#pragma once

// ---- button bit layout (matches th06 TouhouButton; th07 gameplay reads low 9) ----
enum NetButton
{
    NB_SHOOT  = 1 << 0,
    NB_BOMB   = 1 << 1,
    NB_FOCUS  = 1 << 2,
    NB_MENU   = 1 << 3,
    NB_UP     = 1 << 4,
    NB_DOWN   = 1 << 5,
    NB_LEFT   = 1 << 6,
    NB_RIGHT  = 1 << 7,
    NB_SKIP   = 1 << 8,

    NB_SHOOT2 = 1 << 9,
    NB_BOMB2  = 1 << 10,
    NB_FOCUS2 = 1 << 11,
    NB_UP2    = 1 << 12,
    NB_DOWN2  = 1 << 13,
    NB_LEFT2  = 1 << 14,
    NB_RIGHT2 = 1 << 15,
};

// The pure merge function: given each side's raw word, produce the single 16-bit
// word both machines must agree on. is_host selects which side owns the P1 low
// bits; is_in_UI returns self|rcv (menu navigation) instead of the P1/P2 split.
unsigned short MergeKeys(bool is_host, bool is_in_UI,
                         unsigned short self_key, unsigned short rcv_key);
