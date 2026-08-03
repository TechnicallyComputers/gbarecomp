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

bool sio_sample_unpack(const uint8_t* data, std::size_t len, SioSampleView* out) {
    if (!data || !out || len < kSioSampleHeaderSize) return false;
    if (data[2] != kSioSampleVersion) return false;
    const uint8_t count = data[3];
    if (count > kSioSampleMaxEvents) return false;
    if (len < kSioSampleHeaderSize + count * kSioWireEventBytes) return false;

    out->keys = load_u16_le(data);
    out->version = data[2];
    out->count = count;
    for (uint8_t i = 0; i < kSioSampleMaxEvents; ++i) out->events[i] = {};
    for (uint8_t i = 0; i < count; ++i) {
        const uint8_t* e = data + kSioSampleHeaderSize + i * kSioWireEventBytes;
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

// ── FrameLinkPartner ──────────────────────────────────────────────────────

FrameLinkPartner::FrameLinkPartner(uint8_t unit_id)
    : unit_id_(static_cast<uint8_t>(unit_id & 0x3u)) {}

void FrameLinkPartner::begin_frame() {
    outbound_.clear();
    next_seq_ = 0;
    overflow_ = false;
}

std::size_t FrameLinkPartner::sample(uint16_t keys, uint8_t* out,
                                     std::size_t cap) const {
    SioSampleView view{};
    view.keys = keys;
    view.version = kSioSampleVersion;
    view.count = 0;
    for (const SioWireEvent& ev : outbound_) {
        if (view.count >= kSioSampleMaxEvents) break;
        view.events[view.count++] = ev;
    }
    return sio_sample_pack(view, out, cap);
}

bool FrameLinkPartner::publish(const uint8_t* data, std::size_t len) {
    SioSampleView view{};
    if (!sio_sample_unpack(data, len, &view)) return false;
    inbound_.clear();
    for (uint8_t i = 0; i < view.count; ++i)
        inbound_.push_back(view.events[i]);
    return true;
}

void FrameLinkPartner::record_outbound(const SioTransferRequest& req) {
    if (outbound_.size() >= kSioSampleMaxEvents) {
        overflow_ = true;
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
        // External-clock slave: arm only when a peer master event is waiting
        // (delay-sync made their TX visible this frame).
        if (inbound_.empty()) return false;
        if (force_cycles_ && out_cycles)
            *out_cycles = *force_cycles_;
        else if (out_cycles)
            *out_cycles = 64;  // short slave settle once peer data is present
        return true;
    }

    if (req.mode == SioMode::Multi) {
        // Phase 1: both seats may write Start in tests. Real hardware makes
        // Start read-only for children (parent broadcast); dual-core kick of
        // child Start is a later host concern.
        if (force_cycles_ && out_cycles) *out_cycles = *force_cycles_;
        return true;
    }

    return false;
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
            // Peer sample not yet visible (delay gap / first frame).
            out->rx_data = 0xFFFFFFFFu;
        }
        return;
    }

    if (req.mode == SioMode::Multi) {
        for (int i = 0; i < 4; ++i) out->multi[i] = 0xFFFFu;
        out->multi[unit_id_] = static_cast<uint16_t>(req.tx_data & 0xFFFFu);
        // Fold every inbound Multi event into its sender's seat.
        while (!inbound_.empty()) {
            const SioWireEvent ev = inbound_.front();
            inbound_.pop_front();
            if (ev.mode != SioMode::Multi) continue;
            out->multi[ev.unit_id & 0x3u] =
                static_cast<uint16_t>(ev.tx_data & 0xFFFFu);
        }
        out->multi_ready = true;
    }
}

}  // namespace gba
