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

// DIAGNOSTIC: the exact frame index GetKeys read this tick and the raw self/rcv
// words it merged, plus how the peer's input was obtained (0=present immediately,
// 1=arrived after a wait, 2=timed out / defaulted to 0). A host-vs-guest diff of
// (read_frame, self_key, rcv_key) tells a frame-index misalignment apart from a
// stale / mis-delivered peer input.
static int            g_stat_read_frame = -1;
static unsigned short g_stat_self_key   = 0;
static unsigned short g_stat_rcv_key    = 0;
static int            g_stat_rcv_status  = 0;

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

        if (pack.ctrl.ctrl_type == Ctrl_Key)
        {
            int frame = pack.ctrl.frame;
            for (int i = 0; i < KeyPackFrameNum; i++)
            {
                g_ctrl_bits_rcved[frame - i] = pack.ctrl.keys[i];
                g_ctrl_rng_rcved[frame - i]  = pack.ctrl.rng_seed[i];
                g_ctrl_rcved[frame - i]      = pack.ctrl.igc_type[i];
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
    pack.ctrl.ctrl_type = Ctrl_Key;
    pack.ctrl.frame = frame;

    for (int i = 0; i < KeyPackFrameNum; i++)
    {
        std::map<int, Bits<16> >::iterator r = g_ctrl_bits_self.find(frame - i);
        if (r == g_ctrl_bits_self.end()) ReadFromInt(pack.ctrl.keys[i], 0);
        else                             pack.ctrl.keys[i] = r->second;

        std::map<int, int>::iterator r2 = g_ctrl_rng_self.find(frame - i);
        pack.ctrl.rng_seed[i] = (r2 == g_ctrl_rng_self.end()) ? 0
                                                              : (unsigned short)r2->second;

        std::map<int, InGameCtrlType>::iterator r3 = g_ctrl_self.find(frame - i);
        pack.ctrl.igc_type[i] = (r3 == g_ctrl_self.end()) ? IGC_NONE : r3->second;
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

unsigned short Netcode_GetInput_Net(int frame, bool is_in_UI, int& cur_ctrl)
{
    g_netFrame = frame;

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
    g_netFrame = 0;
    g_is_sync = true;
    g_resync_trigger = false;
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

void Netcode_GetReadStats(int& readFrame, unsigned short& selfKey,
                          unsigned short& rcvKey, int& rcvStatus)
{ readFrame = g_stat_read_frame; selfKey = g_stat_self_key;
  rcvKey = g_stat_rcv_key; rcvStatus = g_stat_rcv_status; }

// test-only hooks (defined here, declared in netcode_internal.hpp)
void Netcode_TestSetHost(bool h)  { g_is_host = h; }
