// handshake_test.cpp — 2-process loopback smoke test for the netplay auto-sync.
// Run two copies: one `host`, one `guest`. Verifies the link auto-connects and the
// guest ADOPTS the host's delay+seed (its own differing seed must be overridden).
//   build:  g++ -m32 tests/handshake_test.cpp src/netplay/*.cpp -Isrc/netplay -lws2_32
//   run:    handshake_test.exe host &  ;  handshake_test.exe guest
#include "netcode_c_api.h"
#include <cstdio>
#include <cstring>
#include <windows.h>

static unsigned short ReadIn(void)   { return 0; }
static unsigned short ReadSeed(void) { return 0; }

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: handshake_test host|guest\n"); return 2; }
    bool host = strcmp(argv[1], "host") == 0;

    Nc_SetCallbacks(ReadIn, ReadSeed);
    int ok = host ? Nc_StartHost("", 47100, 2 /*AF_INET*/)
                  : Nc_StartGuest("127.0.0.1", 47100, 47101, 2 /*AF_INET*/);
    if (!ok) { printf("[%s] transport start FAILED\n", argv[1]); return 1; }

    // host chooses delay=3, seed=0x1234; guest starts with a DIFFERENT seed (0x9999)
    // that the handshake must overwrite with the host's.
    unsigned short mySeed = host ? 0x1234 : 0x9999;
    Nc_BeginHandshake(3, mySeed);

    int connected = 0;
    DWORD start = GetTickCount();
    while (GetTickCount() - start < 6000) {
        if (Nc_PumpHandshake()) { connected = 1; break; }
        Sleep(5);
    }
    // keep pumping briefly so the peer also completes
    DWORD t2 = GetTickCount();
    while (GetTickCount() - t2 < 800) { Nc_PumpHandshake(); Sleep(5); }

    printf("[%s] connected=%d delay=%d seed=0x%04x verbad=%d\n",
           argv[1], connected, Nc_GetDelay(), Nc_GetInitSeed(), Nc_HandshakeVersionBad());

    bool seedOk = (Nc_GetInitSeed() == 0x1234);   // both must end on the HOST's seed
    if (!connected) { printf("[%s] FAIL: never connected\n", argv[1]); return 1; }
    if (!seedOk)    { printf("[%s] FAIL: seed not host's 0x1234\n", argv[1]); return 1; }
    printf("[%s] PASS\n", argv[1]);
    return 0;
}
