// link_tests — GBA link-cable / SIO partner (Phase 0 + Phase 1 frame samples).
#include <cstdio>

#include "gba_io.h"
#include "gba_link.h"

namespace {

int failures = 0;

void check_eq(const char* test, const char* tag, unsigned got, unsigned expect) {
    if (got != expect) {
        std::printf("FAIL %s: %s (got 0x%X, expected 0x%X)\n",
                    test, tag, got, expect);
        ++failures;
    }
}

void check_true(const char* test, const char* tag, bool cond) {
    if (!cond) {
        std::printf("FAIL %s: %s\n", test, tag);
        ++failures;
    }
}

void run_normal32(gba::GbaIo& io, uint32_t tx, uint32_t cycles) {
    io.write32(gba::IoReg::SIODATA32, tx);
    io.write16(gba::IoReg::SIOCNT, 0x5081);  // Normal32 internal + Start + IRQ
    io.tick_sio(cycles);
}

void run_multi(gba::GbaIo& io, uint16_t send, uint32_t cycles) {
    io.write16(gba::IoReg::SIOMLT_SEND, send);
    io.write16(gba::IoReg::SIOCNT, 0x6080);  // Multi + Start + IRQ
    io.tick_sio(cycles);
}

// Guest IntrMain write-1-to-clear before SerialCB; tests must do the same
// between Multi rounds or child kicks stay gated on IF.Serial.
void ack_serial(gba::GbaIo& io) {
    io.write16(gba::IoReg::IF, gba::GbaIo::IrqSerial);
}

// Child: Multi mode + send word, no Start (HW Start is parent-only).
void arm_multi_child(gba::GbaIo& io, uint16_t send) {
    io.write16(gba::IoReg::SIOMLT_SEND, send);
    io.write16(gba::IoReg::SIOCNT, 0x6000);  // Multi + IRQ, baud0
}

void test_mode_decode() {
    const char* T = "mode_decode";
    check_eq(T, "normal8",  (unsigned)gba::sio_decode_mode(0x0000, 0x0000),
             (unsigned)gba::SioMode::Normal8);
    check_eq(T, "normal32", (unsigned)gba::sio_decode_mode(0x1000, 0x0000),
             (unsigned)gba::SioMode::Normal32);
    check_eq(T, "multi",    (unsigned)gba::sio_decode_mode(0x2000, 0x0000),
             (unsigned)gba::SioMode::Multi);
    check_eq(T, "uart",     (unsigned)gba::sio_decode_mode(0x3000, 0x0000),
             (unsigned)gba::SioMode::Uart);
    check_eq(T, "gpio",     (unsigned)gba::sio_decode_mode(0x0000, 0x8000),
             (unsigned)gba::SioMode::GeneralPurpose);
    check_eq(T, "joybus",   (unsigned)gba::sio_decode_mode(0x0000, 0xC000),
             (unsigned)gba::SioMode::Joybus);
}

void test_solo_normal_open_bus() {
    const char* T = "solo_normal_open_bus";
    gba::GbaIo io;
    // Normal32, internal 256KHz, IRQ enable, Start.
    // SIOCNT: bit0=1 internal, bit12=1 (32-bit), bit14=1 IRQ, bit7=Start
    io.write16(gba::IoReg::SIOCNT, 0x5081);
    check_true(T, "armed", io.cycles_until_next_sio_event() != 0xFFFFFFFFu);
    check_eq(T, "cycles", io.cycles_until_next_sio_event(), 2048u);

    io.tick_sio(2048);
    check_eq(T, "start cleared", io.read16(gba::IoReg::SIOCNT) & 0x80u, 0u);
    check_eq(T, "open bus", io.read32(gba::IoReg::SIODATA32), 0xFFFFFFFFu);
    check_eq(T, "serial irq", io.if_reg() & gba::GbaIo::IrqSerial,
             (unsigned)gba::GbaIo::IrqSerial);
}

void test_solo_external_never_completes() {
    const char* T = "solo_external";
    gba::GbaIo io;
    // Normal8, external clock, Start (Minish Cap style 0x5088 without bit0).
    io.write16(gba::IoReg::SIOCNT, 0x5080);
    check_eq(T, "not armed", io.cycles_until_next_sio_event(), 0xFFFFFFFFu);
    io.tick_sio(100000);
    check_eq(T, "start stuck", io.read16(gba::IoReg::SIOCNT) & 0x80u, 0x80u);
}

void test_solo_multi_never_completes() {
    const char* T = "solo_multi";
    gba::GbaIo io;
    // Multi mode (bit13=1), baud0, Start + IRQ.
    io.write16(gba::IoReg::SIOCNT, 0x6080);
    check_eq(T, "not armed", io.cycles_until_next_sio_event(), 0xFFFFFFFFu);
}

void test_partner_normal32_rx() {
    const char* T = "partner_normal32";
    gba::GbaIo io;
    gba::ScriptedLinkPartner partner;
    partner.push_normal_rx(0xA5A5A5A5u);
    partner.set_force_cycles(64);
    io.set_link_partner(&partner);

    io.write32(gba::IoReg::SIODATA32, 0x11223344u);
    // Normal32 internal + Start + IRQ
    io.write16(gba::IoReg::SIOCNT, 0x5081);
    check_eq(T, "start_count", (unsigned)partner.start_count(), 1u);
    check_eq(T, "last_tx", partner.last_tx(), 0x11223344u);

    io.tick_sio(64);
    check_eq(T, "rx", io.read32(gba::IoReg::SIODATA32), 0xA5A5A5A5u);
    check_eq(T, "complete_count", (unsigned)partner.complete_count(), 1u);
    check_eq(T, "start cleared", io.read16(gba::IoReg::SIOCNT) & 0x80u, 0u);
}

void test_partner_multi_parent() {
    const char* T = "partner_multi";
    gba::GbaIo io;
    gba::ScriptedLinkPartner partner;
    partner.set_multi_response(0xFFFF, 0x1111, 0x2222, 0x3333, /*id=*/0);
    partner.set_force_cycles(32);
    io.set_link_partner(&partner);

    io.write16(gba::IoReg::SIOMLT_SEND, 0xBEEF);
    // Multi (bit13), Start, IRQ, baud 0
    io.write16(gba::IoReg::SIOCNT, 0x6080);
    check_eq(T, "armed cycles", io.cycles_until_next_sio_event(), 32u);

    io.tick_sio(32);
    check_eq(T, "multi0 echo tx", io.read16(gba::IoReg::SIOMULTI0), 0xBEEFu);
    check_eq(T, "multi1", io.read16(gba::IoReg::SIOMULTI1), 0x1111u);
    check_eq(T, "multi2", io.read16(gba::IoReg::SIOMULTI2), 0x2222u);
    check_eq(T, "multi3", io.read16(gba::IoReg::SIOMULTI3), 0x3333u);
    const uint16_t cnt = io.read16(gba::IoReg::SIOCNT);
    check_eq(T, "start cleared", cnt & 0x80u, 0u);
    check_eq(T, "SD ready", cnt & 0x08u, 0x08u);
    check_eq(T, "id parent", (cnt >> 4) & 0x3u, 0u);
}

void test_sample_roundtrip() {
    const char* T = "sample_roundtrip";
    gba::SioSampleView in{};
    in.keys = 0x03FDu;
    in.count = 2;
    in.events[0].mode = gba::SioMode::Normal32;
    in.events[0].internal_clock = true;
    in.events[0].unit_id = 0;
    in.events[0].seq = 0;
    in.events[0].tx_data = 0xAABBCCDDu;
    in.events[1].mode = gba::SioMode::Multi;
    in.events[1].unit_id = 1;
    in.events[1].seq = 1;
    in.events[1].baud = 3;
    in.events[1].tx_data = 0x1234u;

    uint8_t buf[gba::kSioSampleBytes];
    check_eq(T, "pack size", (unsigned)gba::sio_sample_pack(in, buf, sizeof(buf)),
             (unsigned)gba::kSioSampleBytes);

    gba::SioSampleView out{};
    check_true(T, "unpack", gba::sio_sample_unpack(buf, sizeof(buf), &out));
    check_eq(T, "keys", out.keys, 0x03FDu);
    check_eq(T, "count", out.count, 2u);
    check_eq(T, "e0 tx", out.events[0].tx_data, 0xAABBCCDDu);
    check_eq(T, "e1 mode", (unsigned)out.events[1].mode,
             (unsigned)gba::SioMode::Multi);
    check_eq(T, "e1 id", out.events[1].unit_id, 1u);
    check_eq(T, "e1 baud", out.events[1].baud, 3u);
    check_eq(T, "e1 tx", out.events[1].tx_data, 0x1234u);

    // Bad version rejected.
    buf[2] = 0xFF;
    check_true(T, "bad version", !gba::sio_sample_unpack(buf, sizeof(buf), &out));
}

void test_frame_delay1_normal32() {
    const char* T = "frame_d1_normal32";
    gba::GbaIo io_a, io_b;
    gba::FrameLinkPartner link_a(0), link_b(1);
    link_a.set_force_cycles(16);
    link_b.set_force_cycles(16);
    io_a.set_link_partner(&link_a);
    io_b.set_link_partner(&link_b);

    uint8_t sa[gba::kSioSampleBytes]{};
    uint8_t sb[gba::kSioSampleBytes]{};

    // Frame 0: no inbound yet → open-bus RX; outbound records TX.
    link_a.begin_frame();
    link_b.begin_frame();
    run_normal32(io_a, 0x11111111u, 16);
    run_normal32(io_b, 0x22222222u, 16);
    check_eq(T, "f0 a open bus", io_a.read32(gba::IoReg::SIODATA32), 0xFFFFFFFFu);
    check_eq(T, "f0 b open bus", io_b.read32(gba::IoReg::SIODATA32), 0xFFFFFFFFu);
    check_eq(T, "f0 a out", (unsigned)link_a.outbound_count(), 1u);
    check_eq(T, "f0 b out", (unsigned)link_b.outbound_count(), 1u);
    check_eq(T, "sample a", (unsigned)link_a.sample(0x03FF, sa, sizeof(sa)),
             (unsigned)gba::kSioSampleBytes);
    check_eq(T, "sample b", (unsigned)link_b.sample(0x03FF, sb, sizeof(sb)),
             (unsigned)gba::kSioSampleBytes);

    // Admit: cross-publish (delay = 1 frame of visibility).
    check_true(T, "publish a<-b", link_a.publish(sb, sizeof(sb)));
    check_true(T, "publish b<-a", link_b.publish(sa, sizeof(sa)));

    // Frame 1: each side sees the peer's frame-0 TX as RX.
    link_a.begin_frame();
    link_b.begin_frame();
    run_normal32(io_a, 0x33333333u, 16);
    run_normal32(io_b, 0x44444444u, 16);
    check_eq(T, "f1 a rx", io_a.read32(gba::IoReg::SIODATA32), 0x22222222u);
    check_eq(T, "f1 b rx", io_b.read32(gba::IoReg::SIODATA32), 0x11111111u);
}

void test_frame_delay1_multi() {
    const char* T = "frame_d1_multi";
    gba::GbaIo io_a, io_b;
    gba::FrameLinkPartner link_a(0), link_b(1);
    link_a.set_force_cycles(16);
    link_b.set_force_cycles(16);
    io_a.set_link_partner(&link_a);
    io_b.set_link_partner(&link_b);

    uint8_t sa[gba::kSioSampleBytes]{};
    uint8_t sb[gba::kSioSampleBytes]{};

    // Frame 0: parent Starts; child arms Multi but does not Start.
    link_a.begin_frame();
    link_b.begin_frame();
    run_multi(io_a, 0xA000, 16);
    arm_multi_child(io_b, 0xB001);
    io_b.tick_sio(16);  // no inbound yet → no child transfer
    check_eq(T, "f0 a multi0", io_a.read16(gba::IoReg::SIOMULTI0), 0xA000u);
    check_eq(T, "f0 a multi1", io_a.read16(gba::IoReg::SIOMULTI1), 0xFFFFu);
    check_eq(T, "f0 b out", (unsigned)link_b.outbound_count(), 0u);
    link_a.sample(0x03FF, sa, sizeof(sa));
    link_b.sample(0x03FF, sb, sizeof(sb));
    check_eq(T, "f0 sa ver", sa[2], (unsigned)gba::kSioSampleVersionMulti);
    link_a.publish(sb, sizeof(sb));
    link_b.publish(sa, sizeof(sa));

    // Frame 1: child auto-kicks on published parent word; parent Starts again.
    link_a.begin_frame();
    link_b.begin_frame();
    arm_multi_child(io_b, 0xB001);
    io_b.tick_sio(16);
    check_eq(T, "f1 b multi0", io_b.read16(gba::IoReg::SIOMULTI0), 0xA000u);
    check_eq(T, "f1 b multi1", io_b.read16(gba::IoReg::SIOMULTI1), 0xB001u);
    check_eq(T, "f1 b child SI",
             io_b.read16(gba::IoReg::SIOCNT) & 0x04u, 0x04u);
    ack_serial(io_b);
    run_multi(io_a, 0xA100, 16);
    ack_serial(io_a);
    check_eq(T, "f1 a multi0", io_a.read16(gba::IoReg::SIOMULTI0), 0xA100u);
    // Child's f1 word is not visible to parent until next publish.
    check_eq(T, "f1 a multi1", io_a.read16(gba::IoReg::SIOMULTI1), 0xFFFFu);

    link_a.sample(0x03FF, sa, sizeof(sa));
    link_b.sample(0x03FF, sb, sizeof(sb));
    link_a.publish(sb, sizeof(sb));
    link_b.publish(sa, sizeof(sa));

    // Frame 2: parent sees child's B001; child sees parent's A100.
    link_a.begin_frame();
    link_b.begin_frame();
    arm_multi_child(io_b, 0xB101);
    io_b.tick_sio(16);
    ack_serial(io_b);
    run_multi(io_a, 0xA200, 16);
    ack_serial(io_a);
    check_eq(T, "f2 a multi1", io_a.read16(gba::IoReg::SIOMULTI1), 0xB001u);
    check_eq(T, "f2 b multi0", io_b.read16(gba::IoReg::SIOMULTI0), 0xA100u);
    check_eq(T, "f2 b multi1", io_b.read16(gba::IoReg::SIOMULTI1), 0xB101u);
}

void test_multi_latch_stable_peer() {
    const char* T = "multi_latch";
    gba::GbaIo io_a, io_b;
    gba::FrameLinkPartner link_a(0), link_b(1);
    link_a.set_force_cycles(16);
    link_b.set_force_cycles(16);
    io_a.set_link_partner(&link_a);
    io_b.set_link_partner(&link_b);

    uint8_t sa[gba::kSioSampleBytes]{};
    uint8_t sb[gba::kSioSampleBytes]{};

    // Frame 0: parent TX only.
    link_a.begin_frame();
    link_b.begin_frame();
    run_multi(io_a, 0xB9A0, 16);
    arm_multi_child(io_b, 0xB9A0);
    link_a.sample(0x03FF, sa, sizeof(sa));
    link_b.sample(0x03FF, sb, sizeof(sb));
    link_a.publish(sb, sizeof(sb));
    link_b.publish(sa, sizeof(sa));

    // Frame 1: child responds; parent Starts again (still no child word yet).
    link_a.begin_frame();
    link_b.begin_frame();
    arm_multi_child(io_b, 0xB9A0);
    io_b.tick_sio(16);
    ack_serial(io_b);
    run_multi(io_a, 0xB9A0, 16);
    ack_serial(io_a);
    link_a.sample(0x03FF, sa, sizeof(sa));
    link_b.sample(0x03FF, sb, sizeof(sb));
    link_a.publish(sb, sizeof(sb));
    link_b.publish(sa, sizeof(sa));

    // Frame 2: parent consumes child's B9A0 into the seat latch.
    link_a.begin_frame();
    link_b.begin_frame();
    run_multi(io_a, 0xB9A0, 16);
    ack_serial(io_a);
    check_eq(T, "peer latched", io_a.read16(gba::IoReg::SIOMULTI1), 0xB9A0u);

    // Frame 3: empty inbound — MULTI1 must remain B9A0 while local becomes 8FFF.
    link_a.begin_frame();
    gba::SioSampleView empty_view{};
    empty_view.keys = 0x03FFu;
    uint8_t empty[gba::kSioSampleBytes]{};
    check_eq(T, "empty pack",
             (unsigned)gba::sio_sample_pack(empty_view, empty, sizeof(empty)),
             (unsigned)gba::kSioSampleBytes);
    check_true(T, "empty pub", link_a.publish(empty, sizeof(empty)));
    run_multi(io_a, 0x8FFF, 16);
    ack_serial(io_a);
    check_eq(T, "latched multi0", io_a.read16(gba::IoReg::SIOMULTI0), 0x8FFFu);
    check_eq(T, "latched multi1", io_a.read16(gba::IoReg::SIOMULTI1), 0xB9A0u);
    check_eq(T, "inq drained", (unsigned)link_a.inbound_count(), 0u);
}

void test_multi_dense_pack() {
    const char* T = "multi_dense_pack";
    gba::SioSampleView in{};
    in.keys = 0x03FFu;
    in.count = static_cast<uint8_t>(gba::kSioSampleMaxMultiWords);
    for (uint8_t i = 0; i < in.count; ++i) {
        in.events[i].mode = gba::SioMode::Multi;
        in.events[i].unit_id = 0;
        in.events[i].seq = i;
        in.events[i].tx_data = static_cast<uint32_t>(0xB900u + i);
    }
    uint8_t buf[gba::kSioSampleBytes];
    check_eq(T, "pack", (unsigned)gba::sio_sample_pack(in, buf, sizeof(buf)),
             (unsigned)gba::kSioSampleBytes);
    check_eq(T, "ver", buf[2], (unsigned)gba::kSioSampleVersionMulti);
    check_eq(T, "count nibble", buf[3] & 0x0Fu,
             (unsigned)gba::kSioSampleMaxMultiWords);

    gba::SioSampleView out{};
    check_true(T, "unpack", gba::sio_sample_unpack(buf, sizeof(buf), &out));
    check_eq(T, "count", out.count,
             (unsigned)gba::kSioSampleMaxMultiWords);
    check_eq(T, "first", out.events[0].tx_data, 0xB900u);
    check_eq(T, "last", out.events[13].tx_data, 0xB900u + 13u);
    check_eq(T, "mode", (unsigned)out.events[5].mode,
             (unsigned)gba::SioMode::Multi);
}

void test_multi_cable_sense_before_start() {
    const char* T = "multi_cable_sense";
    gba::GbaIo io_parent, io_child;
    gba::FrameLinkPartner link_p(0), link_c(1);
    io_parent.set_link_partner(&link_p);
    io_child.set_link_partner(&link_c);

    // Arm Multi with no Start — Emerald polls SI/SD here before handshake.
    io_parent.write16(gba::IoReg::SIOCNT, 0x6003);  // Multi + IRQ + 115200
    io_child.write16(gba::IoReg::SIOCNT, 0x6003);

    const uint16_t p = io_parent.read16(gba::IoReg::SIOCNT);
    const uint16_t c = io_child.read16(gba::IoReg::SIOCNT);
    check_eq(T, "parent SI", p & 0x04u, 0u);
    check_eq(T, "parent SD", p & 0x08u, 0x08u);
    check_eq(T, "child SI", c & 0x04u, 0x04u);
    check_eq(T, "child SD", c & 0x08u, 0x08u);
    check_eq(T, "parent id still 0", (p >> 4) & 0x3u, 0u);
    check_eq(T, "child id still 0", (c >> 4) & 0x3u, 0u);

    // Unplugged Multi: SI/SD stay clear (no fake ready).
    gba::GbaIo io_solo;
    io_solo.write16(gba::IoReg::SIOCNT, 0x6003);
    check_eq(T, "solo SD", io_solo.read16(gba::IoReg::SIOCNT) & 0x0Cu, 0u);
}

void test_multi_burst_no_overflow() {
    const char* T = "multi_burst";
    gba::FrameLinkPartner link(0);
    link.set_force_cycles(8);
    gba::GbaIo io;
    io.set_link_partner(&link);
    link.begin_frame();
    for (int i = 0; i < 14; ++i) {
        run_multi(io, static_cast<uint16_t>(0x1000u + i), 8);
        ack_serial(io);
    }
    check_eq(T, "out", (unsigned)link.outbound_count(), 14u);
    check_true(T, "no overflow", !link.overflow());
    run_multi(io, 0x10FFu, 8);
    check_true(T, "overflow on 15", link.overflow());
    check_eq(T, "capped", (unsigned)link.outbound_count(), 14u);

    uint8_t buf[gba::kSioSampleBytes];
    link.sample(0x03FF, buf, sizeof(buf));
    check_eq(T, "v2", buf[2], (unsigned)gba::kSioSampleVersionMulti);
}

// Dense delay-sync samples must not overwrite SIOMULTI before SerialCB.
void test_multi_one_word_per_serial_irq() {
    const char* T = "multi_one_irq";
    gba::GbaIo io_a, io_b;
    gba::FrameLinkPartner link_a(0), link_b(1);
    link_a.set_force_cycles(16);
    link_b.set_force_cycles(16);
    io_a.set_link_partner(&link_a);
    io_b.set_link_partner(&link_b);

    // One Multi/frame while handshaking (pacing); accumulate across frames.
    link_a.begin_frame();
    run_multi(io_a, 0x8FFF, 16);
    ack_serial(io_a);
    link_a.begin_frame();
    run_multi(io_a, 0x0000, 16);
    ack_serial(io_a);
    link_a.begin_frame();
    run_multi(io_a, 0x0000, 16);
    ack_serial(io_a);
    // Re-publish the three words as one sample for the child drain test.
    gba::SioSampleView burst{};
    burst.keys = 0x03FFu;
    burst.count = 3;
    burst.events[0].mode = gba::SioMode::Multi;
    burst.events[0].unit_id = 0;
    burst.events[0].tx_data = 0x8FFFu;
    burst.events[1].mode = gba::SioMode::Multi;
    burst.events[1].unit_id = 0;
    burst.events[1].tx_data = 0x0000u;
    burst.events[2].mode = gba::SioMode::Multi;
    burst.events[2].unit_id = 0;
    burst.events[2].tx_data = 0x0000u;
    uint8_t sa[gba::kSioSampleBytes]{};
    check_true(T, "parent pack 3",
               gba::sio_sample_pack(burst, sa, sizeof(sa)) != 0);
    check_eq(T, "parent packed 3", sa[3] & 0x0Fu, 3u);

    link_b.begin_frame();
    arm_multi_child(io_b, 0xB9A0);
    check_true(T, "child pub", link_b.publish(sa, sizeof(sa)));
    check_eq(T, "inq", (unsigned)link_b.inbound_count(), 3u);

    // One large tick would formerly drain all three before the guest IRQ.
    io_b.tick_sio(100000);
    check_eq(T, "first word multi0", io_b.read16(gba::IoReg::SIOMULTI0),
             0x8FFFu);
    check_eq(T, "first word multi1", io_b.read16(gba::IoReg::SIOMULTI1),
             0xB9A0u);
    check_eq(T, "inq left", (unsigned)link_b.inbound_count(), 2u);
    check_eq(T, "serial if", io_b.if_reg() & gba::GbaIo::IrqSerial,
             (unsigned)gba::GbaIo::IrqSerial);

    // Stuck IF.Serial must block further child kicks.
    io_b.tick_sio(100000);
    check_eq(T, "still first", io_b.read16(gba::IoReg::SIOMULTI0), 0x8FFFu);
    check_eq(T, "inq held", (unsigned)link_b.inbound_count(), 2u);

    ack_serial(io_b);
    // New sim frame — handshake pacing allows one more Multi complete.
    link_b.begin_frame();
    arm_multi_child(io_b, 0xB9A0);
    io_b.tick_sio(16);
    // Zeros pass through so the first DoRecv after handshake sees real CMD data.
    check_eq(T, "zero multi0", io_b.read16(gba::IoReg::SIOMULTI0), 0u);
    check_eq(T, "inq after 2", (unsigned)link_b.inbound_count(), 1u);

    ack_serial(io_b);
    link_b.begin_frame();
    arm_multi_child(io_b, 0x0000);
    io_b.tick_sio(16);
    check_eq(T, "third multi0", io_b.read16(gba::IoReg::SIOMULTI0), 0u);
    check_eq(T, "inq drained", (unsigned)link_b.inbound_count(), 0u);
}

// Delay-sync: stale B9A0 / zeros after 8FFF while child still handshakes.
void test_multi_handshake_latch_hold() {
    const char* T = "multi_hs_hold";
    gba::GbaIo io_b;
    gba::FrameLinkPartner link_b(1);
    link_b.set_force_cycles(16);
    io_b.set_link_partner(&link_b);

    gba::SioSampleView view{};
    view.keys = 0x03FFu;
    view.count = 3;
    view.events[0].mode = gba::SioMode::Multi;
    view.events[0].unit_id = 0;
    view.events[0].tx_data = 0x8FFFu;
    view.events[1].mode = gba::SioMode::Multi;
    view.events[1].unit_id = 0;
    view.events[1].tx_data = 0xB9A0u;  // stale
    view.events[2].mode = gba::SioMode::Multi;
    view.events[2].unit_id = 0;
    view.events[2].tx_data = 0x0000u;
    uint8_t buf[gba::kSioSampleBytes]{};
    check_true(T, "pack",
               gba::sio_sample_pack(view, buf, sizeof(buf)) != 0);

    link_b.begin_frame();
    arm_multi_child(io_b, 0xB9A0);
    check_true(T, "pub", link_b.publish(buf, sizeof(buf)));

    io_b.tick_sio(16);
    check_eq(T, "got master", io_b.read16(gba::IoReg::SIOMULTI0), 0x8FFFu);
    ack_serial(io_b);
    link_b.begin_frame();
    arm_multi_child(io_b, 0xB9A0);
    io_b.tick_sio(16);
    check_eq(T, "stale B9A0 held", io_b.read16(gba::IoReg::SIOMULTI0),
             0x8FFFu);
    ack_serial(io_b);
    link_b.begin_frame();
    arm_multi_child(io_b, 0xB9A0);
    io_b.tick_sio(16);
    // Zeros are real CMD bytes after handshake — must not stick on 8FFF.
    check_eq(T, "zero applied", io_b.read16(gba::IoReg::SIOMULTI0), 0u);
    check_eq(T, "local seat", io_b.read16(gba::IoReg::SIOMULTI1), 0xB9A0u);
}

// Unconsumed Multi words must survive the next frame's publish.
// Parent must not complete CMD zeros in the same frame as MASTER_HANDSHAKE.
void test_multi_handshake_pacing() {
    const char* T = "multi_hs_pace";
    gba::GbaIo io;
    gba::FrameLinkPartner link(0);
    link.set_force_cycles(16);
    io.set_link_partner(&link);

    // Seed peer seat as still handshaking (child B9A0).
    link.begin_frame();
    // Establish local+peer handshake latches via a normal exchange setup:
    // parent B9A0 with published child B9A0.
    {
        gba::SioSampleView peer{};
        peer.keys = 0x03FFu;
        peer.count = 1;
        peer.events[0].mode = gba::SioMode::Multi;
        peer.events[0].unit_id = 1;
        peer.events[0].tx_data = 0xB9A0u;
        uint8_t pb[gba::kSioSampleBytes]{};
        check_true(T, "peer pack",
                   gba::sio_sample_pack(peer, pb, sizeof(pb)) != 0);
        check_true(T, "peer pub", link.publish(pb, sizeof(pb)));
    }
    run_multi(io, 0xB9A0, 16);
    ack_serial(io);
    check_true(T, "pacing after HS", link.multi_handshake_pacing());

    link.begin_frame();
    run_multi(io, 0x8FFF, 16);
    ack_serial(io);
    check_eq(T, "one out", (unsigned)link.outbound_count(), 1u);

    // Same frame: Timer3-style zero must be refused while local is still 8FFF.
    io.write16(gba::IoReg::SIOMLT_SEND, 0x0000);
    io.write16(gba::IoReg::SIOCNT, 0x6080);
    check_eq(T, "start cleared", io.read16(gba::IoReg::SIOCNT) & 0x80u, 0u);
    check_eq(T, "still one out", (unsigned)link.outbound_count(), 1u);
    check_eq(T, "pace rej", link.debug_stats().multi_pace_reject, 1u);

    // Same-frame after 8FFF still paces; next frame trailing B9A0 must not
    // (DoHandshake rewrites SEND → B9A0 before INIT_TIMER / Timer3 burst).
    check_eq(T, "pace while 8FFF local",
             (unsigned)link.multi_handshake_pacing(), 1u);
    link.begin_frame();
    run_multi(io, 0xB9A0, 16);
    ack_serial(io);
    run_multi(io, 0x0000, 16);
    check_eq(T, "post-hs burst", (unsigned)link.outbound_count(), 2u);
    check_eq(T, "pace off after trailing B9A0",
             (unsigned)link.multi_handshake_pacing(), 0u);
}

// After parent leaves handshake, delayed peer B9A0 must not poison DoRecv.
void test_multi_decay_stale_slave_after_hs() {
    const char* T = "multi_decay_slave";
    gba::GbaIo io;
    gba::FrameLinkPartner link(0);
    link.set_force_cycles(16);
    io.set_link_partner(&link);

    gba::SioSampleView peer{};
    peer.keys = 0x03FFu;
    peer.count = 1;
    peer.events[0].mode = gba::SioMode::Multi;
    peer.events[0].unit_id = 1;
    peer.events[0].tx_data = 0xB9A0u;
    uint8_t pb[gba::kSioSampleBytes]{};
    check_true(T, "pack", gba::sio_sample_pack(peer, pb, sizeof(pb)) != 0);

    link.begin_frame();
    check_true(T, "pub", link.publish(pb, sizeof(pb)));
    run_multi(io, 0xB9A0, 16);
    ack_serial(io);
    link.begin_frame();
    run_multi(io, 0x8FFF, 16);
    ack_serial(io);
    check_eq(T, "hs peer", io.read16(gba::IoReg::SIOMULTI1), 0xB9A0u);

    // Trailing DoHandshake B9A0 still keeps the peer latch (local token).
    link.begin_frame();
    check_true(T, "pub2", link.publish(pb, sizeof(pb)));
    run_multi(io, 0xB9A0, 16);
    ack_serial(io);
    check_eq(T, "trailing peer", io.read16(gba::IoReg::SIOMULTI1), 0xB9A0u);

    // First real CMD zero: stale/delayed B9A0 must become 0 for checksum.
    link.begin_frame();
    check_true(T, "pub3", link.publish(pb, sizeof(pb)));
    run_multi(io, 0x0000, 16);
    check_eq(T, "local zero", io.read16(gba::IoReg::SIOMULTI0), 0u);
    check_eq(T, "peer decayed", io.read16(gba::IoReg::SIOMULTI1), 0u);
}

// Parent 8FFF round must keep MULTI1=B9A0 when inbound peers a 0.
void test_multi_hold_zero_over_slave_hs() {
    const char* T = "multi_hold_zero_slave";
    gba::GbaIo io;
    gba::FrameLinkPartner link(0);
    link.set_force_cycles(16);
    io.set_link_partner(&link);

    gba::SioSampleView peer{};
    peer.keys = 0x03FFu;
    peer.count = 2;
    peer.events[0].mode = gba::SioMode::Multi;
    peer.events[0].unit_id = 1;
    peer.events[0].tx_data = 0xB9A0u;
    peer.events[1].mode = gba::SioMode::Multi;
    peer.events[1].unit_id = 1;
    peer.events[1].tx_data = 0x0000u;
    uint8_t pb[gba::kSioSampleBytes]{};
    check_true(T, "pack", gba::sio_sample_pack(peer, pb, sizeof(pb)) != 0);

    link.begin_frame();
    check_true(T, "pub", link.publish(pb, sizeof(pb)));
    run_multi(io, 0xB9A0, 16);
    ack_serial(io);
    check_eq(T, "peer B9A0", io.read16(gba::IoReg::SIOMULTI1), 0xB9A0u);

    link.begin_frame();
    run_multi(io, 0x8FFF, 16);
    check_eq(T, "master", io.read16(gba::IoReg::SIOMULTI0), 0x8FFFu);
    check_eq(T, "slave held", io.read16(gba::IoReg::SIOMULTI1), 0xB9A0u);
}

void test_multi_publish_appends() {
    const char* T = "multi_pub_append";
    gba::FrameLinkPartner link(1);
    gba::SioSampleView a{};
    a.keys = 0x03FFu;
    a.count = 2;
    a.events[0].mode = gba::SioMode::Multi;
    a.events[0].unit_id = 0;
    a.events[0].tx_data = 0x8FFFu;
    a.events[1].mode = gba::SioMode::Multi;
    a.events[1].unit_id = 0;
    a.events[1].tx_data = 0x1111u;
    uint8_t ba[gba::kSioSampleBytes]{};
    uint8_t bb[gba::kSioSampleBytes]{};
    check_true(T, "pack a", gba::sio_sample_pack(a, ba, sizeof(ba)) != 0);

    gba::SioSampleView b = a;
    b.count = 1;
    b.events[0].tx_data = 0x2222u;
    check_true(T, "pack b", gba::sio_sample_pack(b, bb, sizeof(bb)) != 0);

    check_true(T, "pub a", link.publish(ba, sizeof(ba)));
    check_eq(T, "inq 2", (unsigned)link.inbound_count(), 2u);
    check_true(T, "pub b", link.publish(bb, sizeof(bb)));
    check_eq(T, "inq 3", (unsigned)link.inbound_count(), 3u);
}

void test_sample_fits_rnet_input_max() {
    const char* T = "rnet_size";
    // Keep in lockstep with recomp-net RNET_INPUT_MAX without a hard dep.
    check_eq(T, "kSioSampleBytes", (unsigned)gba::kSioSampleBytes, 32u);
    check_eq(T, "max v1 events", (unsigned)gba::kSioSampleMaxEvents, 3u);
    check_eq(T, "max multi words", (unsigned)gba::kSioSampleMaxMultiWords, 14u);
}

}  // namespace

int main() {
    test_mode_decode();
    test_solo_normal_open_bus();
    test_solo_external_never_completes();
    test_solo_multi_never_completes();
    test_partner_normal32_rx();
    test_partner_multi_parent();
    test_multi_cable_sense_before_start();
    test_sample_roundtrip();
    test_sample_fits_rnet_input_max();
    test_multi_dense_pack();
    test_multi_burst_no_overflow();
    test_multi_one_word_per_serial_irq();
    test_multi_handshake_latch_hold();
    test_multi_handshake_pacing();
    test_multi_hold_zero_over_slave_hs();
    test_multi_decay_stale_slave_after_hs();
    test_multi_publish_appends();
    test_frame_delay1_normal32();
    test_frame_delay1_multi();
    test_multi_latch_stable_peer();

    if (failures) {
        std::printf("link_tests: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("link_tests: ok\n");
    return 0;
}
