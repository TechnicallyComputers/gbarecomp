// gba_netplay.cpp — see gba_netplay.h.

#include "gba_netplay.h"

#include <cstdio>
#include <cstring>

#include "gba_link.h"
#include "recomp_net/recomp_net.h"

#if defined(__cplusplus)
static_assert(gba::kSioSampleBytes == RNET_INPUT_MAX,
              "SIO sample must fit recomp-net input blob");
#endif

namespace {

struct GbaNetplayState {
    RNetSession* session = nullptr;
    gba::FrameLinkPartner* link = nullptr;
    uint16_t staged_keys = 0x03FFu;
    int local_slot = 0;
    int active = 0;
};

GbaNetplayState g_np;
GbaNetplayConfig g_pending{};
int g_pending_valid = 0;
int g_return_to_lobby = 0;

void sample_local(rnet_u32 /*tick*/, RNetInputSample* out, void* /*ctx*/) {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    out->valid = 1;
    if (!g_np.link) {
        // Pad-only fallback when no link partner is attached.
        out->bytes[0] = static_cast<rnet_u8>(g_np.staged_keys & 0xFFu);
        out->bytes[1] = static_cast<rnet_u8>((g_np.staged_keys >> 8) & 0xFFu);
        out->bytes[2] = gba::kSioSampleVersion;
        out->bytes[3] = 0;
        out->size = static_cast<rnet_u16>(gba::kSioSampleBytes);
        return;
    }
    const std::size_t n =
        g_np.link->sample(g_np.staged_keys, out->bytes, RNET_INPUT_MAX);
    out->size = static_cast<rnet_u16>(n);
}

void publish(rnet_u32 /*tick*/, const RNetInputSample* by_slot, int slots,
             void* /*ctx*/) {
    if (!g_np.link || !by_slot || slots <= 0) return;
    // Apply every remote seat into the inbound FIFO (2P: the other slot).
    for (int s = 0; s < slots; ++s) {
        if (s == g_np.local_slot) continue;
        if (!by_slot[s].valid || by_slot[s].size == 0) continue;
        g_np.link->publish(by_slot[s].bytes, by_slot[s].size);
    }
}

}  // namespace

extern "C" void gba_netplay_config_defaults(GbaNetplayConfig* cfg) {
    if (!cfg) return;
    std::memset(cfg, 0, sizeof(*cfg));
    cfg->local_slot = 0;
    cfg->input_delay = 2;
    cfg->session_id = 1;
    std::snprintf(cfg->bind_hostport, sizeof(cfg->bind_hostport), ":41000");
}

extern "C" void gba_netplay_set_pending(const GbaNetplayConfig* cfg) {
    if (!cfg || !cfg->enabled) {
        g_pending_valid = 0;
        std::memset(&g_pending, 0, sizeof(g_pending));
        return;
    }
    g_pending = *cfg;
    g_pending.enabled = 1;
    g_pending_valid = 1;
}

extern "C" int gba_netplay_take_pending(GbaNetplayConfig* out) {
    if (!g_pending_valid || !out) return 0;
    *out = g_pending;
    g_pending_valid = 0;
    std::memset(&g_pending, 0, sizeof(g_pending));
    return 1;
}

extern "C" void gba_netplay_set_return_to_lobby(int enabled) {
    g_return_to_lobby = enabled ? 1 : 0;
}

extern "C" int gba_netplay_return_to_lobby_requested(void) {
    return g_return_to_lobby;
}

extern "C" int gba_netplay_consume_return_to_lobby(void) {
    const int v = g_return_to_lobby;
    g_return_to_lobby = 0;
    return v;
}

extern "C" void gba_netplay_set_link_partner(gba::FrameLinkPartner* partner) {
    g_np.link = partner;
}

extern "C" int gba_netplay_active(void) { return g_np.active && g_np.session; }

extern "C" int gba_netplay_is_running(void) {
    return g_np.session && rnet_session_is_running(g_np.session);
}

extern "C" int gba_netplay_local_slot(void) { return g_np.local_slot; }

extern "C" uint32_t gba_netplay_sim_tick(void) {
    return g_np.session ? rnet_session_sim_tick(g_np.session) : 0u;
}

extern "C" int gba_netplay_start_lan(const GbaNetplayConfig* cfg) {
    if (!cfg) return 0;
    gba_netplay_shutdown();

    RNetConfig rc{};
    rc.slot_count = 2;
    rc.local_slot = static_cast<rnet_u8>(cfg->local_slot ? 1 : 0);
    rc.input_delay =
        static_cast<rnet_u8>(cfg->input_delay < 2 ? 2 : cfg->input_delay);
    if (rc.input_delay > 20) rc.input_delay = 20;
    rc.bundle_redundancy = 3;
    rc.session_id = cfg->session_id ? cfg->session_id : 1u;
    rc.protocol_magic = 0x524E4554u;  // "RNET"

    RNetHostVTable vt{};
    vt.sample_local = sample_local;
    vt.publish = publish;
    vt.now_ms = nullptr;
    vt.on_signal = nullptr;
    vt.ctx = nullptr;

    g_np.session = rnet_session_create(&rc, &vt);
    if (!g_np.session) return 0;

    const char* peer =
        (cfg->peer_hostport[0] != '\0') ? cfg->peer_hostport : nullptr;
    if (rnet_session_start_lan(g_np.session, cfg->bind_hostport, peer) != 0) {
        rnet_session_destroy(g_np.session);
        g_np.session = nullptr;
        return 0;
    }

    g_np.local_slot = rc.local_slot;
    g_np.active = 1;
    return 1;
}

extern "C" void gba_netplay_shutdown(void) {
    if (g_np.session) {
        rnet_session_send_bye(g_np.session);
        rnet_session_destroy(g_np.session);
        g_np.session = nullptr;
    }
    g_np.active = 0;
}

extern "C" void gba_netplay_stage_keys(uint16_t keys) {
    g_np.staged_keys = static_cast<uint16_t>(keys & 0x03FFu);
}

extern "C" void gba_netplay_pump(void) {
    if (g_np.session) rnet_session_pump(g_np.session);
}

extern "C" int gba_netplay_poll_admit(void) {
    if (!g_np.session) return 0;
    rnet_session_pump(g_np.session);
    if (!rnet_session_is_running(g_np.session)) return 0;
    const rnet_u32 t = rnet_session_sim_tick(g_np.session);
    // sample_local runs inside try_admit and must see outbound from the
    // previous sim frame. begin_frame() clears that queue — call it only
    // after a successful admit, before the next frame's SIO transfers.
    if (!rnet_session_try_admit(g_np.session, t)) return 0;
    if (g_np.link) g_np.link->begin_frame();
    return 1;
}

extern "C" void gba_netplay_finish_frame(void) {
    if (g_np.session) rnet_session_advance(g_np.session);
}

extern "C" int gba_netplay_input_desync(uint32_t* tick, uint32_t* local_hash,
                                        uint32_t* remote_hash) {
    if (!g_np.session) return 0;
    rnet_u32 t = 0, lh = 0, rh = 0;
    const int d = rnet_session_input_desync(g_np.session, &t, &lh, &rh);
    if (tick) *tick = t;
    if (local_hash) *local_hash = lh;
    if (remote_hash) *remote_hash = rh;
    return d;
}

extern "C" int gba_netplay_peer_disconnected(uint32_t timeout_ms) {
    if (!g_np.session) return 0;
    return rnet_session_peer_disconnected(g_np.session, timeout_ms);
}
