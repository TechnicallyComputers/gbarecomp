// gba_link.h — GBA link-cable / SIO partner surface.
//
// Phase 0: LinkPartner + ScriptedLinkPartner; solo open-bus preserved.
// Phase 1: frame-scoped SIO event queue + opaque sample pack/unpack sized
//          for recomp-net delay-sync (RNET_INPUT_MAX = 32). FrameLinkPartner
//          records local TX on completion and consumes remote TX as RX.
// Phase 1b: Multi-dense sample v2 (14× u16/frame) + parent-driven child Start
//          for Gen3 Cable Club rates (~Timer3 @ 115200).
// Phase 1c: Live Multi SI/SD cable sense while a partner is plugged (GBATEK:
//          games poll SD=ready / SI=parent|child *before* the first Start).
// Phase 1d: Latch last-seen Multi word per seat across completes so delay-sync
//          still presents a stable SIOMULTI0..3 snapshot (Emerald DoHandshake
//          needs playerCount>1 every Serial IRQ, not only when a peer word
//          arrived this frame).
// Phase 1e: While the local seat still TX's a Gen3 handshake token, hold the
//          peer latch against delay-sync stale SLAVE_HANDSHAKE over
//          MASTER_HANDSHAKE, and against 0 over SLAVE_HANDSHAKE (parent
//          DoHandshake needs MULTI1=B9A0 on the 8FFF IRQ). Zeros are not
//          held over MASTER_HANDSHAKE (post-HS DoRecv). publish() appends
//          Multi words (capped) so Timer3 bursts are not discarded.
// Phase 1f: Handshake pacing — 1 Multi/frame during B9A0↔B9A0. After
//          MASTER_HANDSHAKE is seen, pace only while last local TX is still
//          8FFF (same-frame zero refuse); trailing B9A0 / Timer3 CMD runs
//          full rate so LinkVSync's serialIntrCounter≥9 can pass.
// Phase 1g: After local leaves handshake tokens, decay peer SLAVE_HANDSHAKE
//          latches to 0. Delay-sync keeps the child on B9A0 for D frames
//          after the parent enters CONN_ESTABLISHED; presenting that token
//          as CMD data sets pokeemerald badChecksum → CloseLink.
//
// Delay-sync cadence (host-owned; this header is the HW-facing half):
//   begin_frame → (publish remote sample from tick N−D) → run sim/transfers
//   → sample local pad+events → admit/exchange → advance
//
// Mode decode follows GBATEK § "SIO Control Registers Summary":
//   RCNT.15=0, SIOCNT[13:12] = 00 Normal8 / 01 Normal32 / 10 Multi / 11 UART
//   RCNT.15=1, RCNT.14=0 General Purpose; RCNT.15=1,14=1 JOYBUS
//
// References: GBATEK § SIO Normal Mode, § SIO Multi-Player Mode.

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace gba {

enum class SioMode : uint8_t {
    Normal8 = 0,
    Normal32,
    Multi,
    Uart,             // recognized, not completed yet
    GeneralPurpose,   // RCNT GPIO — not a shift transfer
    Joybus,           // recognized, not completed yet
};

// Snapshot of what the local GBA armed when Start rose.
struct SioTransferRequest {
    SioMode  mode           = SioMode::Normal8;
    bool     internal_clock = false;  // Normal: master drives SC
    uint8_t  baud           = 0;      // Multi: SIOCNT[1:0]
    uint32_t tx_data        = 0;      // Normal word or Multi 16-bit send
};

// Partner-supplied result applied into IO on transfer completion.
struct SioTransferResult {
    bool     complete    = false;
    uint32_t rx_data     = 0xFFFFFFFFu;  // Normal 8/32
    uint16_t multi[4]    = {0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu};
    uint8_t  multi_id    = 0;            // 0=parent .. 3
    bool     multi_error = false;
    bool     multi_ready = true;         // SD terminal (all units ready)
    bool     is_child    = false;        // SI terminal (1=child)
};

// Decode SIOCNT+RCNT into a mode. Pure; no IO side effects.
SioMode sio_decode_mode(uint16_t siocnt, uint16_t rcnt);

// GBATEK Normal-mode internal-clock durations (CPU cycles).
uint32_t sio_normal_transfer_cycles(uint16_t siocnt);

// Approximate Multi-Player parent transfer length from baud.
uint32_t sio_multi_transfer_cycles(uint16_t siocnt);

// Gen3 Pokémon Multi handshake tokens (pokeemerald include/link.h).
constexpr uint16_t kMultiMasterHandshake = 0x8FFFu;
constexpr uint16_t kMultiSlaveHandshake  = 0xB9A0u;

inline bool multi_handshake_token(uint16_t word) {
    return word == kMultiMasterHandshake || word == kMultiSlaveHandshake;
}

// True → keep latched peer seat; still consume the inbound wire word.
bool multi_hold_peer_latch(uint16_t latched, uint16_t incoming,
                           uint16_t local_tx);

// ── Phase 1: opaque delay-sync sample ─────────────────────────────────────
// Layout (little-endian), total ≤ kSioSampleBytes (32 = RNET_INPUT_MAX):
//
// v1 (Normal / mixed):
//   [0..1]  KEYINPUT
//   [2]     version = 1
//   [3]     event count (0..kSioSampleMaxEvents)
//   then count × kSioWireEventBytes (8): mode/flags/seq/baud/tx_u32
//
// v2 (Multi-only, Gen3 Cable Club):
//   [0..1]  KEYINPUT
//   [2]     version = 2
//   [3]     flags: bits0-3 = word count (0..14), bits4-5 = sender unit_id
//   then count × u16 LE Multi TX words (one per parent/child Start round)

constexpr std::size_t kSioSampleBytes      = 32;
constexpr std::size_t kSioSampleHeaderSize = 4;
constexpr std::size_t kSioWireEventBytes   = 8;
constexpr std::size_t kSioSampleMaxEvents  =
    (kSioSampleBytes - kSioSampleHeaderSize) / kSioWireEventBytes;  // 3 (v1)
constexpr std::size_t kSioSampleMaxMultiWords =
    (kSioSampleBytes - kSioSampleHeaderSize) / 2;  // 14 (v2)
constexpr uint8_t     kSioSampleVersion       = 1;
constexpr uint8_t     kSioSampleVersionMulti  = 2;

struct SioWireEvent {
    SioMode  mode           = SioMode::Normal8;
    bool     internal_clock = false;
    uint8_t  unit_id        = 0;   // Multi seat of the sender (0..3)
    uint8_t  seq            = 0;
    uint8_t  baud           = 0;
    uint32_t tx_data        = 0;
};

struct SioSampleView {
    uint16_t keys = 0x03FFu;
    uint8_t  version = 0;
    uint8_t  count = 0;
    SioWireEvent events[kSioSampleMaxMultiWords]{};
};

// Pack / unpack. Returns bytes written, or 0 on failure.
// Auto-selects v2 when every event is Multi (else v1, capped at 3).
std::size_t sio_sample_pack(const SioSampleView& view, uint8_t* out,
                            std::size_t cap);
// Returns false if buffer is truncated / bad version.
bool sio_sample_unpack(const uint8_t* data, std::size_t len, SioSampleView* out);

class LinkPartner {
public:
    virtual ~LinkPartner() = default;

    // Called on a Start rising edge the core is willing to arm. `*out_cycles`
    // is pre-filled with the GBATEK default; the partner may overwrite it.
    // Return true to arm the countdown; false leaves Start set but idle
    // (external-clock slave / partner not ready).
    virtual bool on_transfer_start(const SioTransferRequest& req,
                                   uint32_t* out_cycles) = 0;

    // Called when the countdown reaches zero. Fill *out; if out->complete is
    // false the core leaves Start set and keeps waiting.
    virtual void on_transfer_complete(const SioTransferRequest& req,
                                      SioTransferResult* out) = 0;

    // Child Multi seat: when peer parent TX is visible in the inbound FIFO,
    // ask GbaIo to arm a Multi transfer (HW Start is parent-only / read-only
    // on children). Default: never.
    virtual bool poll_multi_slave_start(uint32_t* out_cycles) {
        (void)out_cycles;
        return false;
    }

    // Live Multi-Player SI/SD terminals while the cable is plugged.
    // GBATEK: SI (bit2) 0=parent / 1=child; SD (bit3) 1=all units ready.
    // Return true to overlay these on SIOCNT reads in Multi mode (before any
    // transfer). ID/Error stay transfer-owned. Default: no live sense.
    virtual bool multi_cable_sense(bool* out_is_child, bool* out_all_ready) {
        (void)out_is_child;
        (void)out_all_ready;
        return false;
    }
};

// Test / local double: FIFO of Normal RX words; fixed Multi response.
class ScriptedLinkPartner final : public LinkPartner {
public:
    void push_normal_rx(uint32_t word);
    void set_multi_response(uint16_t m0, uint16_t m1, uint16_t m2, uint16_t m3,
                            uint8_t id = 0, bool error = false);
    void set_force_cycles(std::optional<uint32_t> cycles) {
        force_cycles_ = cycles;
    }

    uint32_t last_tx() const { return last_tx_; }
    SioMode  last_mode() const { return last_mode_; }
    int      start_count() const { return start_count_; }
    int      complete_count() const { return complete_count_; }

    bool on_transfer_start(const SioTransferRequest& req,
                           uint32_t* out_cycles) override;
    void on_transfer_complete(const SioTransferRequest& req,
                              SioTransferResult* out) override;
    bool multi_cable_sense(bool* out_is_child, bool* out_all_ready) override;

private:
    std::deque<uint32_t> normal_rx_;
    uint16_t multi_[4] = {0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu};
    uint8_t  multi_id_ = 0;
    bool     multi_error_ = false;
    std::optional<uint32_t> force_cycles_;
    uint32_t last_tx_ = 0;
    SioMode  last_mode_ = SioMode::Normal8;
    int      start_count_ = 0;
    int      complete_count_ = 0;
};

// Frame-barrier partner for delay-sync: outbound TX events + inbound remote
// TX FIFO. unit_id is this machine's Multi seat (0=parent).
class FrameLinkPartner final : public LinkPartner {
public:
    explicit FrameLinkPartner(uint8_t unit_id = 0);

    void set_unit_id(uint8_t id) { unit_id_ = static_cast<uint8_t>(id & 0x3u); }
    uint8_t unit_id() const { return unit_id_; }

    void set_force_cycles(std::optional<uint32_t> cycles) {
        force_cycles_ = cycles;
    }

    // Clear outbound + per-frame Multi pacing for a new sim tick. Does not
    // touch inbound (inbound is filled by publish from the peer's sample).
    void begin_frame();

    // Pack KEYINPUT + outbound events into out[cap]. Does not clear outbound
    // (call begin_frame next tick). Returns bytes written (0 on failure).
    std::size_t sample(uint16_t keys, uint8_t* out, std::size_t cap) const;

    // Append inbound FIFO with events from a remote sample. Returns false
    // if the buffer is malformed.
    bool publish(const uint8_t* data, std::size_t len);

    // True while Multi completes should be limited to 1/frame (handshake).
    bool multi_handshake_pacing() const;

    std::size_t outbound_count() const { return outbound_.size(); }
    std::size_t inbound_count() const { return inbound_.size(); }
    bool overflow() const { return overflow_; }
    int start_count() const { return start_count_; }
    int complete_count() const { return complete_count_; }

    // Cumulative Multi/handshake diagnostics (not cleared by begin_frame).
    struct DebugStats {
        uint32_t multi_start_ok = 0;
        uint32_t multi_start_reject = 0;
        uint32_t multi_pace_reject = 0;
        uint32_t multi_complete = 0;
        uint32_t slave_kick = 0;
        uint32_t overflow_events = 0;
        uint32_t publish_ok = 0;
        uint32_t publish_fail = 0;
        uint32_t last_sample_ver = 0;
        uint32_t last_sample_count = 0;
        uint32_t last_publish_count = 0;
        uint16_t last_local_tx = 0xFFFFu;
        uint16_t last_peer_rx = 0xFFFFu;
        uint16_t latched_multi[4] = {0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu};
    };
    DebugStats debug_stats() const { return stats_; }

    bool on_transfer_start(const SioTransferRequest& req,
                           uint32_t* out_cycles) override;
    void on_transfer_complete(const SioTransferRequest& req,
                              SioTransferResult* out) override;
    bool poll_multi_slave_start(uint32_t* out_cycles) override;
    bool multi_cable_sense(bool* out_is_child, bool* out_all_ready) override;

private:
    void record_outbound(const SioTransferRequest& req);
    bool has_inbound_multi() const;

    uint8_t unit_id_ = 0;
    uint8_t next_seq_ = 0;
    bool    overflow_ = false;
    uint8_t multi_completes_this_frame_ = 0;
    bool    seen_master_handshake_ = false;
    std::optional<uint32_t> force_cycles_;
    std::deque<SioWireEvent> outbound_;
    std::deque<SioWireEvent> inbound_;
    // Last Multi word observed per seat (HW keeps SIOMULTI until overwritten).
    uint16_t last_multi_[4] = {0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu};
    int start_count_ = 0;
    int complete_count_ = 0;
    mutable DebugStats stats_{};
};

}  // namespace gba
