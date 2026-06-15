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

void Nc_SetLog(NcLogFn fn) { Netcode_SetLog((NetcodeLogFn)fn); }

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

void Nc_BeginHandshake(int delay, unsigned short seed) { Netcode_BeginHandshake(delay, seed); }
int  Nc_PumpHandshake(void)       { return Netcode_PumpHandshake() ? 1 : 0; }
int  Nc_HandshakeVersionBad(void) { return Netcode_HandshakeVersionBad() ? 1 : 0; }

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

void Nc_GetLastSplit(unsigned short* p1, unsigned short* p2)
{
    unsigned short a = 0, b = 0;
    Netcode_GetLastSplit(a, b);
    if (p1) *p1 = a;
    if (p2) *p2 = b;
}

int Nc_GetNetFrame(void) { return Netcode_GetNetFrame(); }

void Nc_GetSyncStats(unsigned short* selfRng, unsigned short* rcvRng, int* waitMs)
{
    unsigned short s = 0, r = 0; int w = 0;
    Netcode_GetSyncStats(s, r, w);
    if (selfRng) *selfRng = s;
    if (rcvRng)  *rcvRng  = r;
    if (waitMs)  *waitMs  = w;
}

void Nc_GetReadStats(int* readFrame, unsigned short* selfKey,
                     unsigned short* rcvKey, int* rcvStatus)
{
    int rf = -1, st = 0; unsigned short sk = 0, rk = 0;
    Netcode_GetReadStats(rf, sk, rk, st);
    if (readFrame) *readFrame = rf;
    if (selfKey)   *selfKey   = sk;
    if (rcvKey)    *rcvKey    = rk;
    if (rcvStatus) *rcvStatus = st;
}

void Nc_GetRcvSrc(int* srcPktFrame, int* writes)
{
    int s = -1, w = 0;
    Netcode_GetRcvSrc(s, w);
    if (srcPktFrame) *srcPktFrame = s;
    if (writes)      *writes      = w;
}

void Nc_GetSendDiag(int* selfRewrites, int* rwFrame,
                    unsigned short* rwOld, unsigned short* rwNew,
                    int* sendZfill, int* zfFrame, int* zfSlot)
{
    int sr = 0, rf = -1, sz = 0, zf = -1, zs = -1; unsigned short ro = 0, rn = 0;
    Netcode_GetSendDiag(sr, rf, ro, rn, sz, zf, zs);
    if (selfRewrites) *selfRewrites = sr;
    if (rwFrame)      *rwFrame      = rf;
    if (rwOld)        *rwOld        = ro;
    if (rwNew)        *rwNew        = rn;
    if (sendZfill)    *sendZfill    = sz;
    if (zfFrame)      *zfFrame      = zf;
    if (zfSlot)       *zfSlot       = zs;
}

} // extern "C"
