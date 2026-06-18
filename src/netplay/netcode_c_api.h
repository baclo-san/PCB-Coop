/* netcode_c_api.h — C-callable shim over the C++ netcode core.
 *
 * The co-op DLL (`src/coop/coop.c`) is C; the netcode (`netcode.cpp`,
 * `Connection.cpp`, `merge.cpp`) is C++ and exposes a C++ API that takes
 * `std::string`. This header is the seam the integration (docs/th07_integration_forkA.md
 * §4) will call: plain C linkage, C-friendly types (const char*, int). The wrappers in
 * `netcode_c_api.cpp` forward to the `Netcode_*` functions 1:1, so this can't drift in
 * behavior from the tested core — it only adapts the calling convention/types.
 *
 * Build: compile `netcode_c_api.cpp` alongside the netcode TUs and link `-lws2_32`.
 */
#ifndef NETCODE_C_API_H
#define NETCODE_C_API_H

#ifdef __cplusplus
extern "C" {
#endif

/* Host callbacks: return the local 16-bit input word / current RNG seed.
 * (th07: readLocalInput -> *(u16*)0x004b9e4c; readRngSeed -> *(u16*)0x0049fe20.) */
typedef unsigned short (*NcReadU16Fn)(void);
void Nc_SetCallbacks(NcReadU16Fn readLocalInput, NcReadU16Fn readRngSeed);

/* DIAGNOSTIC: register a log sink (e.g. coop.c Log). Enables WIRE SEND/RECV lines. */
typedef void (*NcLogFn)(const char*);
void Nc_SetLog(NcLogFn fn);

/* Transport bring-up. family = AF_INET (2) or AF_INET6 (23). Returns 1 on success. */
int  Nc_StartHost(const char* bindIp, int port, int family);
int  Nc_StartGuest(const char* hostIp, int hostPort, int localPort, int family);

/* Mark the link live after the UI handshake; rngSeedInit = host-chosen start seed. */
void Nc_SetConnected(int connected, int delay, unsigned short rngSeedInit);
void Nc_Reset(void);   /* clear per-frame maps (new game / frame-counter reset) */

/* Connection handshake / auto-sync. BeginHandshake arms it (host passes its delay +
 * seed; guest's seed is ignored, it adopts the host's). Call PumpHandshake every
 * front-end frame until it returns 1 (link up). HandshakeVersionBad = peer build
 * mismatch (don't connect). */
void Nc_BeginHandshake(int delay, unsigned short seed);
int  Nc_PumpHandshake(void);
int  Nc_HandshakeVersionBad(void);

/* Auto-resync: opt-in recovery from a sustained in-stage desync. enable!=0 arms it;
 * thresholdFrames = consecutive desynced frames before the host initiates (<=0 keeps the
 * current value). Nc_PollResyncFired returns 1 (once) the frame a realign executed, so the
 * caller reseeds the game RNG to Nc_GetInitSeed() on that frame to converge both sims. */
void Nc_SetAutoResync(int enable, int thresholdFrames);
int  Nc_PollResyncFired(void);

/* Per-frame injection point. Returns the merged 16-bit word both machines agree on
 * for logic-frame `frame`; is_in_UI!=0 -> menu merge (self|rcv), else P1-low/P2-high.
 * out_ctrl receives the resolved in-game control action for this frame. */
unsigned short Nc_GetInputNet(int frame, int is_in_UI, int* out_ctrl);

/* State accessors (all return int 0/1 where boolean). */
int  Nc_IsConnected(void);
int  Nc_IsSync(void);          /* 0 the frame a seed mismatch was detected */
int  Nc_IsHost(void);
int  Nc_GetDelay(void);
unsigned short Nc_GetInitSeed(void);

/* Most recent frame's two raw input words de-merged to player identity (P1 = the
 * host's word, P2 = the guest's word) — for per-player menu routing. Both machines
 * compute the same pair. Valid after Nc_GetInputNet() each frame. */
void Nc_GetLastSplit(unsigned short* p1, unsigned short* p2);

/* Live sync telemetry for the in-game status overlay. Nc_GetNetFrame = the
 * netcode's logic-frame index. Nc_GetSyncStats = the RNG-seed pair last compared
 * (equal => in sync) + ms this frame spent blocked waiting on the peer (climbs as
 * the lockstep stalls; ~0 when healthy). */
int  Nc_GetNetFrame(void);
void Nc_GetSyncStats(unsigned short* selfRng, unsigned short* rcvRng, int* waitMs);

/* DIAGNOSTIC: last frame's GetKeys internals — readFrame = the index it read
 * (netcode frame - delay); selfKey/rcvKey = the raw words merged; rcvStatus =
 * how the peer's input was obtained (0=immediate, 1=after a wait, 2=timeout). */
void Nc_GetReadStats(int* readFrame, unsigned short* selfKey,
                     unsigned short* rcvKey, int* rcvStatus);

/* DIAGNOSTIC: provenance of the received slot read last frame — srcPktFrame = the
 * frame field of the packet that wrote it (-1 if never written); writes = how many
 * packets wrote it. A stale 0 with srcPktFrame>=0 means the guest SENT a 0 there. */
void Nc_GetRcvSrc(int* srcPktFrame, int* writes);

/* DIAGNOSTIC: cumulative send-side fault counters + last event. selfRewrites = a
 * self slot recorded twice with different values; sendZfill = SendKeys zero-filled
 * a recent slot that should have existed. Either being >0 localizes the guest->host
 * drop to a buffer mutation (rewrite) vs a missing/erased slot (zfill). */
void Nc_GetSendDiag(int* selfRewrites, int* rwFrame,
                    unsigned short* rwOld, unsigned short* rwNew,
                    int* sendZfill, int* zfFrame, int* zfSlot);

#ifdef __cplusplus
}
#endif

#endif /* NETCODE_C_API_H */
