// netcode_c_api.cpp — thin C-linkage wrappers over the C++ netcode core.
// Each function forwards 1:1 to its Netcode_* counterpart, adapting only types
// (const char* -> std::string, int <-> bool). No behavior of its own.
#include "netcode_c_api.h"
#include "netcode.hpp"

extern "C" {

void Nc_SetCallbacks(NcReadU16Fn readLocalInput, NcReadU16Fn readRngSeed)
{
    NetcodeCallbacks cb;
    cb.readLocalInput = readLocalInput;
    cb.readRngSeed    = readRngSeed;
    Netcode_SetCallbacks(cb);
}

int Nc_StartHost(const char* bindIp, int port, int family)
{
    return Netcode_StartHost(bindIp ? bindIp : "", port, family) ? 1 : 0;
}

int Nc_StartGuest(const char* hostIp, int hostPort, int localPort, int family)
{
    return Netcode_StartGuest(hostIp ? hostIp : "", hostPort, localPort, family) ? 1 : 0;
}

void Nc_SetConnected(int connected, int delay, unsigned short rngSeedInit)
{
    Netcode_SetConnected(connected != 0, delay, rngSeedInit);
}

void Nc_Reset(void) { Netcode_Reset(); }

unsigned short Nc_GetInputNet(int frame, int is_in_UI, int* out_ctrl)
{
    int ctrl = 0;
    unsigned short w = Netcode_GetInput_Net(frame, is_in_UI != 0, ctrl);
    if (out_ctrl) *out_ctrl = ctrl;
    return w;
}

int Nc_IsConnected(void) { return Netcode_IsConnected() ? 1 : 0; }
int Nc_IsSync(void)      { return Netcode_IsSync()      ? 1 : 0; }
int Nc_IsHost(void)      { return Netcode_IsHost()      ? 1 : 0; }
int Nc_GetDelay(void)    { return Netcode_GetDelay(); }
unsigned short Nc_GetInitSeed(void) { return Netcode_GetInitSeed(); }

} // extern "C"
