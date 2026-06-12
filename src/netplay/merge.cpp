// merge.cpp — the pure input merge (see merge.hpp).
//
// Produce the single 16-bit word BOTH machines compute identically for a frame.
// Extracted verbatim-in-behavior from RUEEE/th06_multi_net Controller::GetKeys.
#include "merge.hpp"

#define TH_ISDOWN(a, mask, b) ((a) & (mask) ? (b) : 0)

unsigned short MergeKeys(bool is_host, bool is_in_UI,
                         unsigned short self_key, unsigned short rcv_key)
{
    if (is_in_UI)
        return (unsigned short)(self_key | rcv_key);

    unsigned short finres = 0;
    if (is_host)
    {
        // P1 = local (host) low bits; P2 = remote (guest) mapped to high bits
        finres = self_key;
        finres |= TH_ISDOWN(rcv_key, NB_LEFT,  NB_LEFT2);
        finres |= TH_ISDOWN(rcv_key, NB_RIGHT, NB_RIGHT2);
        finres |= TH_ISDOWN(rcv_key, NB_UP,    NB_UP2);
        finres |= TH_ISDOWN(rcv_key, NB_DOWN,  NB_DOWN2);
        finres |= TH_ISDOWN(rcv_key, NB_SHOOT, NB_SHOOT2);
        finres |= TH_ISDOWN(rcv_key, NB_BOMB,  NB_BOMB2);
        finres |= TH_ISDOWN(rcv_key, NB_FOCUS, NB_FOCUS2);
        finres |= TH_ISDOWN(rcv_key, NB_MENU,  NB_MENU);
        finres |= TH_ISDOWN(rcv_key, NB_SKIP,  NB_SKIP);
    }
    else
    {
        // P1 = remote (host) low bits; P2 = local (guest) mapped to high bits
        finres = rcv_key;
        finres |= TH_ISDOWN(self_key, NB_LEFT,  NB_LEFT2);
        finres |= TH_ISDOWN(self_key, NB_RIGHT, NB_RIGHT2);
        finres |= TH_ISDOWN(self_key, NB_UP,    NB_UP2);
        finres |= TH_ISDOWN(self_key, NB_DOWN,  NB_DOWN2);
        finres |= TH_ISDOWN(self_key, NB_SHOOT, NB_SHOOT2);
        finres |= TH_ISDOWN(self_key, NB_BOMB,  NB_BOMB2);
        finres |= TH_ISDOWN(self_key, NB_FOCUS, NB_FOCUS2);
        finres |= TH_ISDOWN(self_key, NB_MENU,  NB_MENU);
        finres |= TH_ISDOWN(self_key, NB_SKIP,  NB_SKIP);
    }
    return finres;
}
