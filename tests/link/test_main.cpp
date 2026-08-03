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

    link_a.begin_frame();
    link_b.begin_frame();
    run_multi(io_a, 0xA000, 16);
    run_multi(io_b, 0xB001, 16);
    // Frame 0: only local seat filled.
    check_eq(T, "f0 a multi0", io_a.read16(gba::IoReg::SIOMULTI0), 0xA000u);
    check_eq(T, "f0 a multi1", io_a.read16(gba::IoReg::SIOMULTI1), 0xFFFFu);
    check_eq(T, "f0 b multi1", io_b.read16(gba::IoReg::SIOMULTI1), 0xB001u);
    link_a.sample(0x03FF, sa, sizeof(sa));
    link_b.sample(0x03FF, sb, sizeof(sb));
    link_a.publish(sb, sizeof(sb));
    link_b.publish(sa, sizeof(sa));

    link_a.begin_frame();
    link_b.begin_frame();
    run_multi(io_a, 0xA100, 16);
    run_multi(io_b, 0xB101, 16);
    check_eq(T, "f1 a multi0", io_a.read16(gba::IoReg::SIOMULTI0), 0xA100u);
    check_eq(T, "f1 a multi1", io_a.read16(gba::IoReg::SIOMULTI1), 0xB001u);
    check_eq(T, "f1 b multi0", io_b.read16(gba::IoReg::SIOMULTI0), 0xA000u);
    check_eq(T, "f1 b multi1", io_b.read16(gba::IoReg::SIOMULTI1), 0xB101u);
}

void test_sample_fits_rnet_input_max() {
    const char* T = "rnet_size";
    // Keep in lockstep with recomp-net RNET_INPUT_MAX without a hard dep.
    check_eq(T, "kSioSampleBytes", (unsigned)gba::kSioSampleBytes, 32u);
    check_eq(T, "max events", (unsigned)gba::kSioSampleMaxEvents, 3u);
}

}  // namespace

int main() {
    test_mode_decode();
    test_solo_normal_open_bus();
    test_solo_external_never_completes();
    test_solo_multi_never_completes();
    test_partner_normal32_rx();
    test_partner_multi_parent();
    test_sample_roundtrip();
    test_sample_fits_rnet_input_max();
    test_frame_delay1_normal32();
    test_frame_delay1_multi();

    if (failures) {
        std::printf("link_tests: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("link_tests: ok\n");
    return 0;
}
