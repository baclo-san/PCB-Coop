// netcode_internal.hpp — internals exposed for unit tests only.
#pragma once

// The pure merge function (MergeKeys) now lives in merge.hpp / merge.cpp so it can
// also be tested natively without the Winsock transport (tests/merge_test.cpp).
#include "merge.hpp"

void Netcode_TestSetHost(bool h);
