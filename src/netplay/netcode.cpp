// netcode.cpp — PCB co-op lockstep core.
//
// Port of RUEEE/th06_multi_net Controller.cpp (CC0): delay-buffer + per-frame
// UDP exchange + merge + RNG-seed sync check. th06 engine deps removed:
//   GetInput()          -> g_cb.readLocalInput()
//   g_Rng.seed          -> g_cb.readRngSeed()
//   g_Supervisor.calcCount -> g_netFrame (our own logic-frame index)
// The merge that makes both machines agree on one word is factored into the
// pure function MergeKeys() (declared in netcode_internal.hpp) for unit testing.
#include "netcode.hpp"
#include "netcode_internal.hpp"
#include <map>
#include <cstdio>
#include <cstdarg>

// ---- DIAGNOSTIC log callback (host env supplies; e.g. coop.c's Log) ----
static NetcodeLogFn g_nclog = 0;
void Netcode_SetLog(NetcodeLogFn fn) { g_nclog = fn; }
static void NcLogf(const char* fmt, ...)
{
    if (!g_nclog) return;
    char b[200];
    va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    g_nclog(b);
}

// ---- transport (one peer per process, as in the reference) ----
static Host  g_host;
static Guest g_guest;

// ---- delay-buffer maps, keyed by logic-frame index ----
static std::map<int, Bits<16> >     g_ctrl_bits_self;
static std::map<int, Bits<16> >     g_ctrl_bits_rcved;
static std::map<int, int>           g_ctrl_rng_self;
static std::map<int, int>           g_ctrl_rng_rcved;
static std::map<int, InGameCtrlType> g_ctrl_self;
static std::map<int, InGameCtrlType> g_ctrl_rcved;

// DIAGNOSTIC: provenance of each received bits slot — the frame field of the packet
// that last wrote it, and how many packets wrote it. Tells a guest that SENT a
// zero/wrong value (src == that frame, writes>=1) from a host-side staleness
// (slot never written for this frame => src stays -1).
static std::map<int, int>           g_ctrl_rcved_src;
static std::map<int, int>           g_ctrl_rcved_writes;

// ---- state ----
static NetcodeCallbacks g_cb         = { 0, 0 };
static int   g_delay                 = 1;
static bool  g_is_host               = false;
static bool  g_is_connected          = false;
static bool  g_is_sync               = true;
static bool  g_istry_to_reconnect    = false;
static unsigned short g_initSeed     = 0;
static int   g_netFrame              = 0;   // replaces g_Supervisor.calcCount

static bool  g_resync_trigger        = false;
static int   g_resync_stage_frame    = 0;

// ---- auto-resync (completes the half-ported th06 resync handshake) ----
// When a SUSTAINED desync is detected, the HOST picks an agreed future frame
// (cur + delay*2+2) and announces it via Ctrl_Try_Resync (resent every frame until it
// fires); the guest adopts it on receive (RcvPacks, above). BOTH machines, when their
// own lockstep frame reaches that target, clear the (misaligned) peer-input buffer and
// latch g_resync_did_fire so the host environment can reseed the game RNG to a common
// value — realigning the two sims at the SAME logic frame without a full scene reload.
// This is the AUTOMATIC form of the manual "Escape->Give up->Retry" recovery. Inert
// while synced (only counts/acts once g_is_sync has been false for g_resync_threshold
// consecutive connected frames), so it cannot perturb a healthy run. Default OFF in the
// core; the host env (coop.c) opts in via Netcode_SetAutoResync so the native tests stay
// deterministic. Mirrors RUEEE Supervisor.cpp:124-164.
// ---- host-authoritative difficulty (keep both installs' STARTING config identical) ----
// Each machine reports its current difficulty on every Ctrl_Key packet; the guest forces
// its difficulty global to the host's. Out-of-band metadata, not lockstep state. -1 = none.
static int   g_local_diff            = -1;     // this machine's difficulty (set by host env each frame)
static int   g_peer_diff             = -1;     // last difficulty received from the peer

static bool  g_resync_enable         = false;
static int   g_resync_threshold      = 180;   // sustained desync frames before host acts
static int   g_resync_desync_run     = 0;     // consecutive connected & !sync frames
static bool  g_resync_did_fire       = false; // latched: a realign just executed (env polls)
static int   g_resync_fire_count     = 0;     // diag: total realigns this session

// Scene generation. The netcode frame index resets to 0 at every scene boundary
// (the start barrier re-zeroes it so the two machines realign after the asymmetric
// menu/stage load). That makes frame numbers REUSED across scenes — so a peer's
// trailing inputs from the previous scene (e.g. Z held while confirming a character
// in char-select) carry frame numbers that, after the reset, collide with the next
// scene's early frames. Those stale packets are still in the peer's send window /
// in flight, and without a generation tag the receiver writes them into the new
// scene's identically-numbered rcved slots — injecting phantom inputs the sender
// never made for that scene (the menu->stage desync: 3 phantom P2 shots at
// stage-frame == char-select-length). g_epoch++ on each reset; outgoing packets are
// stamped with it; RcvPacks drops Ctrl_Key packets from an older generation.
static unsigned int g_epoch          = 0;

// Most recent frame's two raw input words, de-merged to player identity (P1 = the
// HOST's word, P2 = the GUEST's word) so the front-end can route per-player menu
// input. Updated in GetKeys every connected frame; same on both machines.
static unsigned short g_last_p1      = 0;
static unsigned short g_last_p2      = 0;

// Live sync telemetry (for the in-game status overlay). Updated each connected
// frame in GetKeys: the RNG-seed pair we last compared, and how long this frame
// blocked waiting for the peer's input — the direct read on a lockstep stall.
static unsigned short g_stat_self_rng = 0;
static unsigned short g_stat_rcv_rng  = 0;
static int            g_stat_wait_ms   = 0;

// Ping / RTT (item 3). Echo model, no clock sync needed: every outgoing pack carries
// sendTick = our GetTickCount64() and echoTick = the peer's most-recent sendTick we saw.
// When a pack comes back carrying echoTick, that value is OUR earlier sendTick, so
// rtt = now - echoTick is the full round-trip measured entirely in our own clock.
static unsigned long long g_peer_last_sendTick = 0;   // peer tick to echo back
static int                g_stat_ping_ms       = -1;  // smoothed RTT, ms (-1 = unknown)

// DIAGNOSTIC: the exact frame index GetKeys read this tick and the raw self/rcv
// words it merged, plus how the peer's input was obtained (0=present immediately,
// 1=arrived after a wait, 2=timed out / defaulted to 0). A host-vs-guest diff of
// (read_frame, self_key, rcv_key) tells a frame-index misalignment apart from a
// stale / mis-delivered peer input.
static int            g_stat_read_frame = -1;
static unsigned short g_stat_self_key   = 0;
static unsigned short g_stat_rcv_key    = 0;
static int            g_stat_rcv_status  = 0;
static int            g_stat_rcv_src     = -1;  // packet frame that wrote the slot read
static int            g_stat_rcv_writes  = 0;   // # packets that wrote that slot

// DIAGNOSTIC: two send-side fault detectors for the guest->host drop.
//  - self_rewrites: g_ctrl_bits_self[frame] written twice with DIFFERENT values
//    (would mean the same logic frame is sampled/recorded more than once).
//  - send_zfill: SendKeys had to zero-fill a RECENT slot (frame-i, i<=delay+2)
//    because it was missing from the buffer (premature erase / never recorded).
// The last event of each is kept so the host env can log it.
static int            g_diag_self_rewrites = 0;
static int            g_diag_rw_frame      = -1;
static unsigned short g_diag_rw_old        = 0;
static unsigned short g_diag_rw_new        = 0;
static int            g_diag_send_zfill    = 0;
static int            g_diag_zf_frame      = -1;
static int            g_diag_zf_slot       = -1;

// The merge (MergeKeys) — the single word both machines compute identically — now
// lives in merge.cpp so it can be unit-tested natively (see merge.hpp).

// ---------------------------------------------------------------------------
static bool RcvPacks()
{
    bool hasdata_all = false;
    bool hasdata;
    do
    {
        Pack pack;
        if (g_is_host) g_host.PollReceive(pack, hasdata);
        else           g_guest.PollReceive(pack, hasdata);

        hasdata_all |= hasdata;
        if (!hasdata)
            return hasdata_all;

        // PING/RTT (item 3): remember the peer's clock to echo, and if this pack echoes
        // OUR tick back, the round-trip is now - echoTick (our clock). EWMA-smooth it.
        // Out-of-band — never touches the lockstep input/RNG. Done for ALL packs so the
        // readout stays live even across scene-stale (epoch-dropped) key packets.
        if (pack.sendTick) g_peer_last_sendTick = pack.sendTick;
        if (pack.echoTick) {
            unsigned long long now = GetTickCount64();
            if (now >= pack.echoTick) {
                int rtt = (int)(now - pack.echoTick);
                g_stat_ping_ms = (g_stat_ping_ms < 0) ? rtt : (g_stat_ping_ms * 3 + rtt) / 4;
            }
        }

        if (pack.ctrl.ctrl_type == Ctrl_Key)
        {
            // EPOCH GUARD — drop a previous scene-generation's key packet. Frame
            // indices reset to 0 each scene, so a stale packet's frame numbers alias
            // this scene's slots; writing it would inject the peer's old-scene inputs
            // (the menu->stage phantom-shot desync). Newer epochs are accepted (a peer
            // that crossed the scene boundary a frame ahead of us); our own imminent
            // reset clears anything it deposits, and the peer re-sends post-reset.
            if (pack.epoch < g_epoch)
            {
                static int s_lastStaleEpoch = -1;
                if ((int)pack.epoch != s_lastStaleEpoch)
                {
                    NcLogf("EPOCH DROP stale key pkt epoch=%u < cur=%u (frame=%d)",
                           pack.epoch, g_epoch, pack.ctrl.frame);
                    s_lastStaleEpoch = (int)pack.epoch;
                }
                continue;   // drain the next datagram; do NOT write rcved slots
            }
            // peer's current difficulty (out-of-band; the guest forces its global to the host's)
            if (pack.ctrl.sender_diff >= 0) g_peer_diff = pack.ctrl.sender_diff;
            int frame = pack.ctrl.frame;
            for (int i = 0; i < KeyPackFrameNum; i++)
            {
                int slot = frame - i;
                // DIAGNOSTIC: log the first NON-ZERO arrival of each slot — i.e. the
                // raw byte the wire actually delivered for the peer's input. Compare
                // to the peer's WIRE SEND line for the same slot: if the host RECVs a
                // bit the guest never SENT, it was injected in transit/recv, not the
                // guest buffer.
                unsigned short kv = 0; WriteToInt(pack.ctrl.keys[i], kv);
                if (kv != 0 && g_ctrl_rcved_writes.find(slot) == g_ctrl_rcved_writes.end())
                    NcLogf("WIRE RECV pkt=%d slot=%d val=%04x", frame, slot, kv);

                g_ctrl_bits_rcved[slot] = pack.ctrl.keys[i];
                g_ctrl_rng_rcved[slot]  = pack.ctrl.rng_seed[i];
                g_ctrl_rcved[slot]      = pack.ctrl.igc_type[i];
                g_ctrl_rcved_src[slot]    = frame;   // DIAGNOSTIC: source pkt frame
                g_ctrl_rcved_writes[slot] += 1;
            }
        }
        else if (pack.ctrl.ctrl_type == Ctrl_Try_Resync)
        {
            if ((pack.ctrl.resync_setting.frame_to_re_sync > g_netFrame) &&
                (pack.ctrl.resync_setting.frame_to_re_sync <= g_netFrame + g_delay * 2 + 2))
            {
                g_resync_trigger = true;
                g_resync_stage_frame = pack.ctrl.resync_setting.frame_to_re_sync;
            }
        }
    } while (hasdata);
    return hasdata_all;
}

static void SendKeys(int frame)
{
    Pack pack;
    pack.type = 4;
    pack.epoch = g_epoch;                 // stamp the current scene generation
    pack.sendTick = GetTickCount64();     // ping (item 3): our clock now
    pack.echoTick = g_peer_last_sendTick; // ...and echo the peer's latest tick back
    pack.ctrl.ctrl_type = Ctrl_Key;
    pack.ctrl.frame = frame;
    pack.ctrl.sender_diff = g_local_diff; // our current difficulty (guest forces to host's)

    for (int i = 0; i < KeyPackFrameNum; i++)
    {
        std::map<int, Bits<16> >::iterator r = g_ctrl_bits_self.find(frame - i);
        if (r == g_ctrl_bits_self.end()) {
            ReadFromInt(pack.ctrl.keys[i], 0);
            // DIAGNOSTIC: a RECENT slot is missing → we are transmitting a 0 the
            // peer will treat as real input. frame-0 is this frame (just recorded);
            // anything within delay+2 should still exist.
            if (i <= g_delay + 2 && (frame - i) >= 0) {
                g_diag_send_zfill++;
                g_diag_zf_frame = frame; g_diag_zf_slot = frame - i;
            }
        }
        else                             pack.ctrl.keys[i] = r->second;

        std::map<int, int>::iterator r2 = g_ctrl_rng_self.find(frame - i);
        pack.ctrl.rng_seed[i] = (r2 == g_ctrl_rng_self.end()) ? 0
                                                              : (unsigned short)r2->second;

        std::map<int, InGameCtrlType>::iterator r3 = g_ctrl_self.find(frame - i);
        pack.ctrl.igc_type[i] = (r3 == g_ctrl_self.end()) ? IGC_NONE : r3->second;
    }

    // DIAGNOSTIC: log this frame's OWN input (keys[0] = self[frame]) when non-zero,
    // deduped by (frame,value) so the host's frequent re-pings don't spam. This is
    // the canonical first transmission of frame F's input; compare to the peer's
    // WIRE RECV slot=F line to see if the wire preserved it.
    {
        unsigned short k0 = 0; WriteToInt(pack.ctrl.keys[0], k0);
        static int s_lastF = -2; static unsigned short s_lastV = 0xffff;
        if (k0 != 0 && (frame != s_lastF || k0 != s_lastV)) {
            NcLogf("WIRE SEND frame=%d key0=%04x", frame, k0);
            s_lastF = frame; s_lastV = k0;
        }
    }

    if (g_is_host) g_host.SendPack(pack);
    else           g_guest.SendPack(pack);
}

static unsigned short GetKeys(int frame, bool is_in_UI, int& out_ctrl)
{
    InGameCtrlType self_ctrl = IGC_NONE;
    InGameCtrlType rcv_ctrl  = IGC_NONE;
    out_ctrl = IGC_NONE;

    if (frame - g_delay < 0)
        return 0;

    unsigned short self_key = 0;
    std::map<int, Bits<16> >::iterator res = g_ctrl_bits_self.find(frame - g_delay);
    if (res != g_ctrl_bits_self.end())
        WriteToInt(res->second, self_key);

    std::map<int, InGameCtrlType>::iterator res2 = g_ctrl_self.find(frame - g_delay);
    if (res2 != g_ctrl_self.end())
        self_ctrl = res2->second;

    unsigned short rcv_key = 0;
    bool has_rcv_data = false;
    bool waited = false;

    static bool inited = false;
    static LARGE_INTEGER freq;
    LARGE_INTEGER cur, ping_key_time, max_wait_to_time;
    if (!inited) { inited = true; QueryPerformanceFrequency(&freq); }
    QueryPerformanceCounter(&cur);
    LARGE_INTEGER wait_start = cur;
    max_wait_to_time.QuadPart = cur.QuadPart + (LONGLONG)(freq.QuadPart * 5.0);   // 5s lockstep stall
    ping_key_time.QuadPart    = cur.QuadPart + (LONGLONG)(freq.QuadPart * 0.1);

    do {
        res = g_ctrl_bits_rcved.find(frame - g_delay);
        if (res != g_ctrl_bits_rcved.end())
        {
            WriteToInt(res->second, rcv_key);
            unsigned short sr = (unsigned short)g_ctrl_rng_self[frame - g_delay];
            unsigned short rr = (unsigned short)g_ctrl_rng_rcved[frame - g_delay];
            g_is_sync = (rr == sr);
            g_stat_self_rng = sr;
            g_stat_rcv_rng  = rr;
            rcv_ctrl = g_ctrl_rcved[frame - g_delay];
            has_rcv_data = true;
            break;
        }
        else
        {
            waited = true;
            while (cur.QuadPart < max_wait_to_time.QuadPart)
            {
                if (RcvPacks()) { Sleep(1); break; }
                Sleep(1);
                QueryPerformanceCounter(&cur);
                if (cur.QuadPart > ping_key_time.QuadPart)
                {
                    ping_key_time.QuadPart = cur.QuadPart + (LONGLONG)(freq.QuadPart * 0.1);
                    SendKeys(frame);       // re-ping our keys so the peer doesn't lock
                }
            }
        }
    } while (cur.QuadPart < max_wait_to_time.QuadPart);

    g_stat_wait_ms = (int)((cur.QuadPart - wait_start.QuadPart) * 1000 / freq.QuadPart);

    g_stat_read_frame = frame - g_delay;
    g_stat_self_key   = self_key;
    g_stat_rcv_key    = rcv_key;
    g_stat_rcv_status = has_rcv_data ? (waited ? 1 : 0) : 2;
    {
        std::map<int, int>::iterator si = g_ctrl_rcved_src.find(frame - g_delay);
        g_stat_rcv_src = (si != g_ctrl_rcved_src.end()) ? si->second : -1;
        std::map<int, int>::iterator wi = g_ctrl_rcved_writes.find(frame - g_delay);
        g_stat_rcv_writes = (wi != g_ctrl_rcved_writes.end()) ? wi->second : 0;
    }

    if (!has_rcv_data)
    {
        rcv_key = self_key = 0;
        self_ctrl = rcv_ctrl = IGC_NONE;
        g_is_connected = false;
        g_istry_to_reconnect = false;
    }

    if (self_ctrl != IGC_NONE && rcv_ctrl != IGC_NONE)
        out_ctrl = g_is_host ? self_ctrl : rcv_ctrl;
    else
        out_ctrl = (self_ctrl == IGC_NONE) ? rcv_ctrl : self_ctrl;

    // de-merge to player identity for per-player menu routing (P1 = host's word).
    g_last_p1 = g_is_host ? self_key : rcv_key;
    g_last_p2 = g_is_host ? rcv_key  : self_key;

    return MergeKeys(g_is_host, is_in_UI, self_key, rcv_key);
}

// HandleControlKeys equivalent: the core carries no local cheat keys yet.
// (Protocol slot stays so igc_type round-trips; integration can populate this.)
static void HandleControlKeys(int frame)
{
    g_ctrl_self[frame] = IGC_NONE;
}

// Host announces the agreed resync target to the guest. Resent every frame while the
// trigger is armed so the guest catches it once its own frame enters the accept window
// (RcvPacks: target in (g_netFrame, g_netFrame + delay*2+2]). Epoch-stamped like all
// frame-4 packets so the receiver's epoch guard treats it as current-generation.
static void SendResyncRequest(int targetFrame)
{
    Pack p;
    p.type     = 4;
    p.epoch    = g_epoch;
    p.seq      = 0;
    p.sendTick = GetTickCount64();
    p.echoTick = 0;
    p.ctrl.ctrl_type = Ctrl_Try_Resync;
    p.ctrl.resync_setting.frame_to_re_sync = targetFrame;
    if (g_is_host) g_host.SendPack(p); else g_guest.SendPack(p);
}

// The resync state machine, run at the top of each connected logic frame (mirrors the
// resync block at the head of EoSD Supervisor::OnUpdate, before GetInput_Net). g_is_sync
// here reflects the PREVIOUS frame's seed compare, which is what we want to act on.
static void RunResync(int frame, bool is_in_UI)
{
    // Resync is meaningful IN-STAGE only: the front-end menus are not RNG-locked (the two
    // machines reach a screen from slightly different states), so g_is_sync flaps there and
    // is not a real fork. Skip entirely in UI — matches coop.c judging desync in-stage only.
    if (!g_resync_enable || !g_is_connected || is_in_UI) { g_resync_desync_run = 0; return; }

    // sustained-desync counter — transient ≤15-frame blips self-heal at scene/seed
    // boundaries (handoff §8q), so only a long unbroken run is a real fork worth resyncing.
    if (g_is_sync) g_resync_desync_run = 0;
    else           g_resync_desync_run++;

    // EXECUTE — both machines realign when their own frame reaches the agreed target.
    // Clearing the rcved maps drops the misaligned peer inputs; the peer (executing the
    // same frame) re-sends its recent window via SendKeys, so the buffers re-fill aligned.
    if (g_resync_trigger && g_resync_stage_frame <= frame)
    {
        RcvPacks();
        g_ctrl_bits_rcved.clear();
        g_ctrl_rng_rcved.clear();
        g_ctrl_rcved.clear();
        g_ctrl_rcved_src.clear();
        g_ctrl_rcved_writes.clear();
        g_is_sync           = true;
        g_resync_trigger    = false;
        g_resync_desync_run = 0;
        g_resync_did_fire   = true;     // env polls Netcode_PollResyncFired -> reseed game RNG
        g_resync_fire_count++;
        NcLogf("RESYNC #%d executed at frame %d (cleared peer buffer; reseed pending)",
               g_resync_fire_count, frame);
        return;
    }

    // HOST INITIATE — only after a sustained desync, and only if no resync is pending.
    // The target is far enough ahead (delay*2+2) that the guest can receive it and reach
    // it; resent each frame until it fires (the same SendResyncRequest the guest accepts).
    if (g_is_host && !g_is_sync && g_resync_desync_run >= g_resync_threshold)
    {
        if (!g_resync_trigger)
        {
            g_resync_stage_frame = frame + g_delay * 2 + 2;
            g_resync_trigger     = true;
            NcLogf("RESYNC host-initiated: target frame %d (desync run %d)",
                   g_resync_stage_frame, g_resync_desync_run);
        }
        SendResyncRequest(g_resync_stage_frame);
    }
}

unsigned short Netcode_GetInput_Net(int frame, bool is_in_UI, int& cur_ctrl)
{
    g_netFrame = frame;
    RunResync(frame, is_in_UI);   // auto-resync handshake (inert unless enabled & sustained-desynced in-stage)

    if (!g_is_connected)
    {
        unsigned short input = g_cb.readLocalInput ? g_cb.readLocalInput() : 0;
        HandleControlKeys(frame);
        cur_ctrl = g_ctrl_self[frame];
        return input;
    }

    unsigned short btn = g_cb.readLocalInput ? g_cb.readLocalInput() : 0;
    Bits<16> cur_btn_bits;
    ReadFromInt(cur_btn_bits, btn);
    // DIAGNOSTIC: catch a same-frame rewrite with a DIFFERENT value — i.e. this
    // logic frame's self slot being recorded more than once (the suspected cause of
    // the guest sending values that disagree with its own later readback).
    {
        std::map<int, Bits<16> >::iterator ex = g_ctrl_bits_self.find(frame);
        if (ex != g_ctrl_bits_self.end()) {
            unsigned short ov = 0; WriteToInt(ex->second, ov);
            if (ov != btn) {
                g_diag_self_rewrites++;
                g_diag_rw_frame = frame; g_diag_rw_old = ov; g_diag_rw_new = btn;
            }
        }
    }
    g_ctrl_bits_self[frame] = cur_btn_bits;
    g_ctrl_rng_self[frame]  = g_cb.readRngSeed ? g_cb.readRngSeed() : 0;

    // erase frames older than 80
    const int frame_rem = 80;
    g_ctrl_bits_self.erase(frame - frame_rem);
    g_ctrl_bits_rcved.erase(frame - frame_rem);
    g_ctrl_rng_rcved.erase(frame - frame_rem);
    g_ctrl_rng_self.erase(frame - frame_rem);
    g_ctrl_rcved.erase(frame - frame_rem);
    g_ctrl_self.erase(frame - frame_rem);
    g_ctrl_rcved_src.erase(frame - frame_rem);
    g_ctrl_rcved_writes.erase(frame - frame_rem);

    HandleControlKeys(frame);
    SendKeys(frame);
    RcvPacks();

    return GetKeys(frame, is_in_UI, cur_ctrl);
}

// ---------------------------------------------------------------------------
// lifecycle / accessors
// ---------------------------------------------------------------------------
void Netcode_SetCallbacks(const NetcodeCallbacks& cb) { g_cb = cb; }

bool Netcode_StartHost(const std::string& bindIp, int port, int family)
{
    g_is_host = true;
    return g_host.Start(bindIp, port, family);
}

bool Netcode_StartGuest(const std::string& hostIp, int hostPort, int localPort, int family)
{
    g_is_host = false;
    return g_guest.Start(hostIp, hostPort, localPort, family);
}

void Netcode_SetConnected(bool connected, int delay, unsigned short rngSeedInit)
{
    g_is_connected = connected;
    if (delay > 0) g_delay = delay;
    g_initSeed = rngSeedInit;
    g_is_sync = true;
}

// ---- connection handshake (headless port of ConnectionUI's PING/PONG + InitSetting) ----
// Replaces the optimistic immediate-connect: the link only goes live once the PEER
// answers, and the HOST's delay+seed are pushed to the guest over the wire (so the
// players no longer have to hand-match seed= in both inis — the host's wins).
enum { PK_PING = 2, PK_PONG = 3 };
static bool               g_hs_active    = false;   // handshake in progress
static unsigned int       g_hs_seq       = 0;
static unsigned long long g_hs_last_ping = 0;
static bool               g_hs_ver_bad   = false;   // peer build/version mismatch (latched)

static void HsSend(int packType, unsigned int seq)
{
    Pack p;
    p.type     = packType;
    p.epoch    = g_epoch;
    p.seq      = seq;
    p.sendTick = GetTickCount64();
    p.echoTick = 0;
    p.ctrl.ctrl_type = Ctrl_Set_InitSetting;
    p.ctrl.init_setting.delay         = g_delay;
    p.ctrl.init_setting.ver           = MULTI_NET_VER;
    p.ctrl.init_setting.rng_seed_init = g_initSeed;   // host's chosen seed (guest's is ignored)
    if (g_is_host) g_host.SendPack(p); else g_guest.SendPack(p);
}

void Netcode_BeginHandshake(int delay, unsigned short seed)
{
    if (delay > 0) g_delay = delay;
    g_initSeed     = seed;       // host: chosen; guest: fallback until it adopts the host's
    g_is_connected = false;
    g_is_sync      = true;
    g_hs_active    = true;
    g_hs_seq       = 0;
    g_hs_last_ping = 0;
    g_hs_ver_bad   = false;
}

bool Netcode_HandshakeVersionBad() { return g_hs_ver_bad; }

// Call once per front-end frame until it returns true. Drives the PING/PONG so the
// link comes up on its own; the guest adopts the host's delay+seed. Idempotent once
// connected. (Host::SendPack no-ops until it has heard from the guest, so the host's
// pings start landing only after the guest's first ping teaches it the address.)
bool Netcode_PumpHandshake()
{
    if (g_is_connected) return true;
    if (!g_hs_active)    return false;

    unsigned long long now = GetTickCount64();
    if (g_hs_last_ping == 0 || now - g_hs_last_ping >= 200) {   // ~5 pings/sec
        HsSend(PK_PING, g_hs_seq++);
        g_hs_last_ping = now;
    }

    for (;;) {
        Pack p; bool hasData = false;
        bool ok = g_is_host ? g_host.PollReceive(p, hasData)
                            : g_guest.PollReceive(p, hasData);
        if (!ok || !hasData) break;

        if (p.ctrl.ctrl_type == Ctrl_Set_InitSetting &&
            p.ctrl.init_setting.ver != MULTI_NET_VER) {
            g_hs_ver_bad = true;            // different build — refuse to connect
            continue;
        }

        if (p.type == PK_PING)             // answer the peer's ping with our settings
            HsSend(PK_PONG, p.seq);

        // The guest adopts the host's delay+seed from any InitSetting it receives
        // (its only peer IS the host). The host keeps its own and ignores the guest's.
        if (!g_is_host && p.ctrl.ctrl_type == Ctrl_Set_InitSetting) {
            if (p.ctrl.init_setting.delay > 0) g_delay = p.ctrl.init_setting.delay;
            g_initSeed = p.ctrl.init_setting.rng_seed_init;
        }

        g_is_connected = true;             // a PING or PONG from the peer proves the link
    }

    if (g_is_connected) g_hs_active = false;
    return g_is_connected;
}

void Netcode_Reset()
{
    g_ctrl_bits_self.clear();
    g_ctrl_bits_rcved.clear();
    g_ctrl_rng_self.clear();
    g_ctrl_rng_rcved.clear();
    g_ctrl_self.clear();
    g_ctrl_rcved.clear();
    g_ctrl_rcved_src.clear();
    g_ctrl_rcved_writes.clear();
    g_netFrame = 0;
    g_is_sync = true;
    g_resync_trigger = false;
    g_resync_desync_run = 0;   // a scene boundary is its own realign; start the counter fresh
    g_resync_did_fire = false; // drop any un-polled latch (the reset reseeds anyway)
    g_epoch++;   // new scene generation — older packets are now stale (epoch guard)
    // NB: g_diag_* counters are intentionally NOT reset here — they are cumulative
    // across the whole session so a scene change can't hide a fault.
}

// ---- host-authoritative difficulty (env sets local each frame; guest reads peer's) ----
void Netcode_SetLocalDifficulty(int diff) { g_local_diff = diff; }
int  Netcode_GetPeerDifficulty()          { return g_peer_diff; }

// ---- auto-resync control / poll (host env opts in; polls the realign latch) ----
void Netcode_SetAutoResync(bool enable, int thresholdFrames)
{
    g_resync_enable = enable;
    if (thresholdFrames > 0) g_resync_threshold = thresholdFrames;
}
// Returns 1 (and clears the latch) iff a realign EXECUTED since the last poll, so the
// host environment can reseed the game RNG to a common value on that exact frame.
int Netcode_PollResyncFired()
{
    if (!g_resync_did_fire) return 0;
    g_resync_did_fire = false;
    return 1;
}

bool Netcode_IsConnected()        { return g_is_connected; }
bool Netcode_IsSync()             { return g_is_sync; }
bool Netcode_IsHost()             { return g_is_host; }
int  Netcode_GetDelay()           { return g_delay; }
unsigned short Netcode_GetInitSeed() { return g_initSeed; }
void Netcode_GetLastSplit(unsigned short& p1, unsigned short& p2)
{ p1 = g_last_p1; p2 = g_last_p2; }

int Netcode_GetNetFrame() { return g_netFrame; }
void Netcode_GetSyncStats(unsigned short& selfRng, unsigned short& rcvRng, int& waitMs)
{ selfRng = g_stat_self_rng; rcvRng = g_stat_rcv_rng; waitMs = g_stat_wait_ms; }
int Netcode_GetPing() { return g_stat_ping_ms; }   // smoothed RTT ms (item 3); -1 = unknown

void Netcode_GetReadStats(int& readFrame, unsigned short& selfKey,
                          unsigned short& rcvKey, int& rcvStatus)
{ readFrame = g_stat_read_frame; selfKey = g_stat_self_key;
  rcvKey = g_stat_rcv_key; rcvStatus = g_stat_rcv_status; }

void Netcode_GetRcvSrc(int& srcPktFrame, int& writes)
{ srcPktFrame = g_stat_rcv_src; writes = g_stat_rcv_writes; }

void Netcode_GetSendDiag(int& selfRewrites, int& rwFrame,
                         unsigned short& rwOld, unsigned short& rwNew,
                         int& sendZfill, int& zfFrame, int& zfSlot)
{ selfRewrites = g_diag_self_rewrites; rwFrame = g_diag_rw_frame;
  rwOld = g_diag_rw_old; rwNew = g_diag_rw_new;
  sendZfill = g_diag_send_zfill; zfFrame = g_diag_zf_frame; zfSlot = g_diag_zf_slot; }

// test-only hooks (defined here, declared in netcode_internal.hpp)
void Netcode_TestSetHost(bool h)  { g_is_host = h; }
