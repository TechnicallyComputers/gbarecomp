// gba_link.cpp — see gba_link.h.

#include "gba_link.h"

#include <cstring>

namespace gba {
namespace {

void store_u16_le(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

void store_u32_le(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

uint16_t load_u16_le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t load_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) |
                                 (p[3] << 24));
}

bool view_is_multi_only(const SioSampleView& view) {
    if (view.count == 0) return false;
    for (uint8_t i = 0; i < view.count; ++i) {
        if (view.events[i].mode != SioMode::Multi) return false;
    }
    return true;
}

std::size_t pack_v1(const SioSampleView& view, uint8_t* out) {
    const uint8_t count =
        view.count > kSioSampleMaxEvents ? kSioSampleMaxEvents : view.count;
    std::memset(out, 0, kSioSampleBytes);
    store_u16_le(out, view.keys);
    out[2] = kSioSampleVersion;
    out[3] = count;
    for (uint8_t i = 0; i < count; ++i) {
        uint8_t* e = out + kSioSampleHeaderSize + i * kSioWireEventBytes;
        const SioWireEvent& ev = view.events[i];
        e[0] = static_cast<uint8_t>(ev.mode);
        e[1] = static_cast<uint8_t>((ev.internal_clock ? 0x01u : 0u) |
                                    ((ev.unit_id & 0x3u) << 1));
        e[2] = ev.seq;
        e[3] = ev.baud;
        store_u32_le(e + 4, ev.tx_data);
    }
    return kSioSampleBytes;
}

std::size_t pack_v2_multi(const SioSampleView& view, uint8_t* out) {
    const uint8_t count = view.count > kSioSampleMaxMultiWords
                              ? static_cast<uint8_t>(kSioSampleMaxMultiWords)
                              : view.count;
    const uint8_t unit =
        count > 0 ? static_cast<uint8_t>(view.events[0].unit_id & 0x3u) : 0;
    std::memset(out, 0, kSioSampleBytes);
    store_u16_le(out, view.keys);
    out[2] = kSioSampleVersionMulti;
    out[3] = static_cast<uint8_t>((count & 0x0Fu) | ((unit & 0x3u) << 4));
    for (uint8_t i = 0; i < count; ++i) {
        store_u16_le(out + kSioSampleHeaderSize + i * 2,
                     static_cast<uint16_t>(view.events[i].tx_data & 0xFFFFu));
    }
    return kSioSampleBytes;
}

}  // namespace

SioMode sio_decode_mode(uint16_t siocnt, uint16_t rcnt) {
    // GBATEK § "SIO Control Registers Summary" mode table.
    if (rcnt & 0x8000u) {
        return (rcnt & 0x4000u) ? SioMode::Joybus : SioMode::GeneralPurpose;
    }
    switch ((siocnt >> 12) & 0x3u) {
        case 0:  return SioMode::Normal8;
        case 1:  return SioMode::Normal32;
        case 2:  return SioMode::Multi;
        default: return SioMode::Uart;
    }
}

uint32_t sio_normal_transfer_cycles(uint16_t siocnt) {
    static constexpr uint32_t kSioCycles[4] = {512u, 64u, 2048u, 256u};
    const uint32_t idx = ((siocnt >> 1) & 1u) | ((siocnt >> 11) & 2u);
    return kSioCycles[idx];
}

bool multi_hold_peer_latch(uint16_t latched, uint16_t incoming,
                           uint16_t local_tx) {
    // Only while we still look like Gen3 DoHandshake locally.
    if (!multi_handshake_token(local_tx))
        return false;
    // Delay-sync can deliver a late SLAVE_HANDSHAKE frame after MASTER.
    if (incoming == kMultiSlaveHandshake && latched == kMultiMasterHandshake)
        return true;
    // Keep peer SLAVE_HANDSHAKE visible when a 0 sneaks into the same round
    // as MASTER_HANDSHAKE — DoHandshake treats any non-FFFF non-token as
    // playerCount=0 and the parent then falls back to B9A0 forever.
    // Do not hold zeros over MASTER_HANDSHAKE (post-HS DoRecv needs real CMD).
    if (incoming == 0x0000u && latched == kMultiSlaveHandshake)
        return true;
    return false;
}

uint32_t sio_multi_transfer_cycles(uint16_t siocnt) {
    // Rough parent-frame lengths at each baud (CPU cycles). Tuned for
    // deterministic IRQ tests, not cable bit-accuracy.
    static constexpr uint32_t kMultiCycles[4] = {
        2048u * 16u,  // 9600
        512u * 16u,   // 38400
        342u * 16u,   // 57600
        171u * 16u,   // 115200
    };
    return kMultiCycles[siocnt & 0x3u];
}

std::size_t sio_sample_pack(const SioSampleView& view, uint8_t* out,
                            std::size_t cap) {
    if (!out || cap < kSioSampleBytes) return 0;
    if (view_is_multi_only(view)) return pack_v2_multi(view, out);
    return pack_v1(view, out);
}

bool sio_sample_unpack(const uint8_t* data, std::size_t len, SioSampleView* out) {
    if (!data || !out || len < kSioSampleHeaderSize) return false;
    const uint8_t ver = data[2];
    out->keys = load_u16_le(data);
    out->version = ver;
    for (std::size_t i = 0; i < kSioSampleMaxMultiWords; ++i) out->events[i] = {};

    if (ver == kSioSampleVersion) {
        const uint8_t count = data[3];
        if (count > kSioSampleMaxEvents) return false;
        if (len < kSioSampleHeaderSize + count * kSioWireEventBytes) return false;
        out->count = count;
        for (uint8_t i = 0; i < count; ++i) {
            const uint8_t* e =
                data + kSioSampleHeaderSize + i * kSioWireEventBytes;
            SioWireEvent& ev = out->events[i];
            ev.mode = static_cast<SioMode>(e[0]);
            ev.internal_clock = (e[1] & 0x01u) != 0;
            ev.unit_id = static_cast<uint8_t>((e[1] >> 1) & 0x3u);
            ev.seq = e[2];
            ev.baud = e[3];
            ev.tx_data = load_u32_le(e + 4);
        }
        return true;
    }

    if (ver == kSioSampleVersionMulti) {
        const uint8_t count = static_cast<uint8_t>(data[3] & 0x0Fu);
        const uint8_t unit = static_cast<uint8_t>((data[3] >> 4) & 0x3u);
        if (count > kSioSampleMaxMultiWords) return false;
        if (len < kSioSampleHeaderSize + count * 2u) return false;
        out->count = count;
        for (uint8_t i = 0; i < count; ++i) {
            SioWireEvent& ev = out->events[i];
            ev.mode = SioMode::Multi;
            ev.internal_clock = true;
            ev.unit_id = unit;
            ev.seq = i;
            ev.baud = 3;  // 115200 typical for Gen3
            ev.tx_data = load_u16_le(data + kSioSampleHeaderSize + i * 2);
        }
        return true;
    }

    return false;
}

// ── ScriptedLinkPartner ───────────────────────────────────────────────────

void ScriptedLinkPartner::push_normal_rx(uint32_t word) {
    normal_rx_.push_back(word);
}

void ScriptedLinkPartner::set_multi_response(uint16_t m0, uint16_t m1,
                                             uint16_t m2, uint16_t m3,
                                             uint8_t id, bool error) {
    multi_[0] = m0;
    multi_[1] = m1;
    multi_[2] = m2;
    multi_[3] = m3;
    multi_id_ = static_cast<uint8_t>(id & 0x3u);
    multi_error_ = error;
}

bool ScriptedLinkPartner::on_transfer_start(const SioTransferRequest& req,
                                            uint32_t* out_cycles) {
    ++start_count_;
    last_tx_ = req.tx_data;
    last_mode_ = req.mode;

    if (req.mode == SioMode::Normal8 || req.mode == SioMode::Normal32) {
        if (!req.internal_clock) return false;
        if (force_cycles_ && out_cycles) *out_cycles = *force_cycles_;
        return true;
    }

    if (req.mode == SioMode::Multi) {
        if (force_cycles_ && out_cycles) *out_cycles = *force_cycles_;
        return true;
    }

    return false;
}

void ScriptedLinkPartner::on_transfer_complete(const SioTransferRequest& req,
                                               SioTransferResult* out) {
    ++complete_count_;
    out->complete = true;
    out->multi_ready = true;
    out->is_child = false;
    out->multi_error = false;

    if (req.mode == SioMode::Normal8 || req.mode == SioMode::Normal32) {
        if (!normal_rx_.empty()) {
            out->rx_data = normal_rx_.front();
            normal_rx_.pop_front();
        } else {
            out->rx_data = 0xFFFFFFFFu;
        }
        return;
    }

    if (req.mode == SioMode::Multi) {
        for (int i = 0; i < 4; ++i) out->multi[i] = multi_[i];
        if (out->multi[0] == 0xFFFFu)
            out->multi[0] = static_cast<uint16_t>(req.tx_data & 0xFFFFu);
        out->multi_id = multi_id_;
        out->multi_error = multi_error_;
        out->is_child = multi_id_ != 0;
    }
}

bool ScriptedLinkPartner::multi_cable_sense(bool* out_is_child,
                                            bool* out_all_ready) {
    if (out_is_child) *out_is_child = multi_id_ != 0;
    if (out_all_ready) *out_all_ready = true;
    return true;
}

// ── FrameLinkPartner ──────────────────────────────────────────────────────

FrameLinkPartner::FrameLinkPartner(uint8_t unit_id)
    : unit_id_(static_cast<uint8_t>(unit_id & 0x3u)) {}

void FrameLinkPartner::begin_frame() {
    outbound_.clear();
    next_seq_ = 0;
    overflow_ = false;
    multi_completes_this_frame_ = 0;
}

bool FrameLinkPartner::multi_handshake_pacing() const {
    // After MASTER_HANDSHAKE: keep 1/frame only while the last local TX is
    // still 8FFF (refuse same-frame Timer3 zeros sharing that sample). Once
    // SEND falls back to B9A0 (DoHandshake) or CMD words, full Timer3 rate.
    if (seen_master_handshake_)
        return stats_.last_local_tx == kMultiMasterHandshake;
    const uint8_t self = static_cast<uint8_t>(unit_id_ & 0x3u);
    return multi_handshake_token(stats_.last_local_tx) ||
           multi_handshake_token(last_multi_[self]);
}

bool FrameLinkPartner::has_inbound_multi() const {
    for (const SioWireEvent& ev : inbound_) {
        if (ev.mode == SioMode::Multi) return true;
    }
    return false;
}

std::size_t FrameLinkPartner::sample(uint16_t keys, uint8_t* out,
                                     std::size_t cap) const {
    SioSampleView view{};
    view.keys = keys;
    view.version = kSioSampleVersion;
    view.count = 0;
    for (const SioWireEvent& ev : outbound_) {
        if (view.count >= kSioSampleMaxMultiWords) break;
        view.events[view.count++] = ev;
    }
    const std::size_t n = sio_sample_pack(view, out, cap);
    if (n && out) {
        stats_.last_sample_ver = out[2];
        stats_.last_sample_count =
            (out[2] == kSioSampleVersionMulti)
                ? static_cast<uint32_t>(out[3] & 0x0Fu)
                : static_cast<uint32_t>(out[3]);
    }
    return n;
}

bool FrameLinkPartner::publish(const uint8_t* data, std::size_t len) {
    SioSampleView view{};
    if (!sio_sample_unpack(data, len, &view)) {
        ++stats_.publish_fail;
        return false;
    }
    // Append this frame's wire words. Replacing the queue dropped unconsumed
    // Multi words when the parent Timer3-burst (>1 word/frame) outran the
    // child's one-Serial-IRQ-per-complete drain — losing MASTER_HANDSHAKE
    // trailers / CMD payload under delay-sync.
    constexpr std::size_t kInboundCap = kSioSampleMaxMultiWords * 4u;
    for (uint8_t i = 0; i < view.count; ++i)
        inbound_.push_back(view.events[i]);
    while (inbound_.size() > kInboundCap)
        inbound_.pop_front();
    ++stats_.publish_ok;
    stats_.last_publish_count = view.count;
    return true;
}

void FrameLinkPartner::record_outbound(const SioTransferRequest& req) {
    const std::size_t cap = (req.mode == SioMode::Multi)
                                ? kSioSampleMaxMultiWords
                                : kSioSampleMaxEvents;
    if (outbound_.size() >= cap) {
        overflow_ = true;
        ++stats_.overflow_events;
        return;
    }
    SioWireEvent ev{};
    ev.mode = req.mode;
    ev.internal_clock = req.internal_clock;
    ev.unit_id = unit_id_;
    ev.seq = next_seq_++;
    ev.baud = req.baud;
    ev.tx_data = req.tx_data;
    outbound_.push_back(ev);
}

bool FrameLinkPartner::on_transfer_start(const SioTransferRequest& req,
                                         uint32_t* out_cycles) {
    ++start_count_;

    if (req.mode == SioMode::Normal8 || req.mode == SioMode::Normal32) {
        if (req.internal_clock) {
            if (force_cycles_ && out_cycles) *out_cycles = *force_cycles_;
            return true;
        }
        // External-clock slave: arm only when a peer master event is waiting.
        if (inbound_.empty()) return false;
        if (force_cycles_ && out_cycles)
            *out_cycles = *force_cycles_;
        else if (out_cycles)
            *out_cycles = 64;
        return true;
    }

    if (req.mode == SioMode::Multi) {
        // Parent (unit 0) may Start anytime. Child Start is HW-read-only —
        // arm only when a parent Multi word is already published (delay-sync).
        if (unit_id_ != 0 && !has_inbound_multi()) {
            ++stats_.multi_start_reject;
            return false;
        }
        if (multi_handshake_pacing() && multi_completes_this_frame_ >= 1u) {
            ++stats_.multi_pace_reject;
            ++stats_.multi_start_reject;
            return false;
        }
        if (force_cycles_ && out_cycles) *out_cycles = *force_cycles_;
        ++stats_.multi_start_ok;
        return true;
    }

    return false;
}

bool FrameLinkPartner::poll_multi_slave_start(uint32_t* out_cycles) {
    if (unit_id_ == 0) return false;
    if (!has_inbound_multi()) return false;
    if (force_cycles_ && out_cycles)
        *out_cycles = *force_cycles_;
    else if (out_cycles)
        *out_cycles = sio_multi_transfer_cycles(0x0003u);  // 115200
    ++stats_.slave_kick;
    return true;
}

bool FrameLinkPartner::multi_cable_sense(bool* out_is_child,
                                         bool* out_all_ready) {
    // Netplay attaches this partner only when a peer seat exists; treat the
    // cable as fully ready. SI reflects our Multi seat (parent=0).
    if (out_is_child) *out_is_child = unit_id_ != 0;
    if (out_all_ready) *out_all_ready = true;
    return true;
}

void FrameLinkPartner::on_transfer_complete(const SioTransferRequest& req,
                                            SioTransferResult* out) {
    ++complete_count_;
    record_outbound(req);

    out->complete = true;
    out->multi_ready = true;
    out->multi_error = false;
    out->multi_id = unit_id_;
    out->is_child = unit_id_ != 0;

    if (req.mode == SioMode::Normal8 || req.mode == SioMode::Normal32) {
        if (!inbound_.empty()) {
            out->rx_data = inbound_.front().tx_data;
            inbound_.pop_front();
        } else {
            out->rx_data = 0xFFFFFFFFu;
        }
        return;
    }

    if (req.mode == SioMode::Multi) {
        ++stats_.multi_complete;
        if (multi_completes_this_frame_ < 255u)
            ++multi_completes_this_frame_;
        const uint16_t local =
            static_cast<uint16_t>(req.tx_data & 0xFFFFu);
        stats_.last_local_tx = local;
        last_multi_[unit_id_ & 0x3u] = local;
        if (local == kMultiMasterHandshake)
            seen_master_handshake_ = true;

        // One new peer word per round (keeps Timer3 CMD bursts 1:1), but
        // always present the latched full cable snapshot. Emerald's
        // DoHandshake needs MULTI0+MULTI1 valid on *every* Serial IRQ —
        // clearing the peer slot when delay-sync has no fresh word drops
        // playerCount to 1 and makes CheckShouldAdvanceLinkState a no-op.
        for (auto it = inbound_.begin(); it != inbound_.end(); ++it) {
            if (it->mode != SioMode::Multi) continue;
            const uint8_t id = static_cast<uint8_t>(it->unit_id & 0x3u);
            uint16_t peer = static_cast<uint16_t>(it->tx_data & 0xFFFFu);
            stats_.last_peer_rx = peer;
            // Parent CONN_ESTABLISHED while the delayed child still TX's
            // B9A0: treat that token as an empty CMD word, not payload.
            if (seen_master_handshake_ && !multi_handshake_token(local) &&
                peer == kMultiSlaveHandshake) {
                peer = 0;
            }
            if (!multi_hold_peer_latch(last_multi_[id], peer, local))
                last_multi_[id] = peer;
            if (last_multi_[id] == kMultiMasterHandshake)
                seen_master_handshake_ = true;
            inbound_.erase(it);
            break;
        }

        // No fresh peer word this round: still drop a stale B9A0 latch once
        // we're past handshake so DoRecv checksum rounds see 0/0.
        if (seen_master_handshake_ && !multi_handshake_token(local)) {
            for (uint8_t i = 0; i < 4; ++i) {
                if (i == (unit_id_ & 0x3u)) continue;
                if (last_multi_[i] == kMultiSlaveHandshake)
                    last_multi_[i] = 0;
            }
        }

        for (int i = 0; i < 4; ++i) {
            out->multi[i] = last_multi_[i];
            stats_.latched_multi[i] = last_multi_[i];
            if (last_multi_[i] == kMultiMasterHandshake)
                seen_master_handshake_ = true;
        }
        out->multi_ready = true;
    }
}

}  // namespace gba
