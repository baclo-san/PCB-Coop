// netcode.hpp — public interface for the PCB co-op netcode core.
//
// This is the th06 mod's Controller lockstep logic (delay buffer + merge + sync)
// decoupled from th06's engine. The original Controller::GetInput() read th06's
// DirectInput keyboard; here the host environment supplies two callbacks:
//   - ReadLocalInput : returns this frame's raw local 16-bit button word
//                      (th07: read g_InputMenu @0x004b9e4c; test: scripted)
//   - ReadRngSeed    : returns the current RNG seed for the per-frame sync check
//                      (th07: *(u16*)0x0049fe20; test: a simulated value)
//
// Per-frame flow lives in Netcode_GetInput_Net(); see netcode.cpp / Controller.cpp.
#pragma once
#include "Connection.hpp"
#include "merge.hpp"   // NetButton bit layout + the pure MergeKeys() (platform-independent)

// host-provided callbacks
typedef unsigned short (*ReadLocalInputFn)(void);
typedef unsigned short (*ReadRngSeedFn)(void);

struct NetcodeCallbacks
{
    ReadLocalInputFn readLocalInput;
    ReadRngSeedFn    readRngSeed;
};

// ---- lifecycle ----
void Netcode_SetCallbacks(const NetcodeCallbacks& cb);

// Bring up transport. host: bindIp may be "" (any), port to listen on.
// guest: peer host ip/port + local bind port. family AF_INET or AF_INET6.
bool Netcode_StartHost(const std::string& bindIp, int port, int family);
bool Netcode_StartGuest(const std::string& hostIp, int hostPort, int localPort, int family);

// Mark the link live (after the UI handshake completes) and set the agreed delay.
// rngSeedInit is the host-chosen start seed both machines must load at game start.
void Netcode_SetConnected(bool connected, int delay, unsigned short rngSeedInit);
void Netcode_Reset();          // clear all per-frame maps (call on new game / calcCount reset)

// ---- connection handshake (auto-sync the link + push the host's seed/delay) ----
// BeginHandshake arms it (host: its delay+seed; guest: delay fallback, adopts host's).
// PumpHandshake is called every front-end frame until it returns true (link up); the
// guest ends with the host's delay+seed. HandshakeVersionBad latches on a build mismatch.
void Netcode_BeginHandshake(int delay, unsigned short seed);
bool Netcode_PumpHandshake();
bool Netcode_HandshakeVersionBad();

// ---- per-frame entry (the injection point) ----
// Returns the merged 16-bit word BOTH machines agree on for logic-frame `frame`:
//   gameplay (is_in_UI=false): P1 = host's low-bit input, P2 = guest's input in high bits.
//   menu     (is_in_UI=true) : self_key | rcv_key (both navigate together).
// out_ctrl receives the resolved in-game control action (cheats/quit/restart) for this frame.
unsigned short Netcode_GetInput_Net(int frame, bool is_in_UI, int& out_ctrl);

// ---- state accessors ----
bool Netcode_IsConnected();
bool Netcode_IsSync();          // false the frame a seed mismatch was detected
bool Netcode_IsHost();
int  Netcode_GetDelay();
unsigned short Netcode_GetInitSeed();

// Most recent frame's two raw input words de-merged to player identity (P1 = the
// host's word, P2 = the guest's word). For per-player front-end (menu) routing;
// both machines compute the same pair. Valid after Netcode_GetInput_Net() each frame.
void Netcode_GetLastSplit(unsigned short& p1, unsigned short& p2);

// Live sync telemetry for the in-game status overlay. GetNetFrame = the netcode's
// own logic-frame index. GetSyncStats = the RNG-seed pair last compared for the
// desync oracle, plus the ms this frame spent blocked waiting for the peer's input
// (climbs toward the 5s timeout when the lockstep is stalling).
int  Netcode_GetNetFrame();
void Netcode_GetSyncStats(unsigned short& selfRng, unsigned short& rcvRng, int& waitMs);

// DIAGNOSTIC: GetKeys internals from the last frame — the read frame index
// (frame - delay), the raw self/rcv words it merged, and how the peer's input
// was obtained (0=immediate, 1=after a wait, 2=timed out/defaulted).
void Netcode_GetReadStats(int& readFrame, unsigned short& selfKey,
                          unsigned short& rcvKey, int& rcvStatus);
