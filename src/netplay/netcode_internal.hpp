// netcode_internal.hpp — internals exposed for unit tests only.
#pragma once

// The pure merge function: given each side's raw word, produce the single
// 16-bit word both machines must agree on. is_host selects which side owns the
// P1 low bits; is_in_UI returns self|rcv (menu) instead of the P1/P2 split.
unsigned short MergeKeys(bool is_host, bool is_in_UI,
                         unsigned short self_key, unsigned short rcv_key);

void Netcode_TestSetHost(bool h);
