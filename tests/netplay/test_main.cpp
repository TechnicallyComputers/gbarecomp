// netplay_smoke_tests — recomp-net CMake wiring + gba_netplay facade smoke.
#include <cstdio>

#include "gba_io.h"
#include "gba_link.h"
#include "gba_netplay.h"
#include "recomp_net/recomp_net.h"

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

void test_sizes_align() {
    const char* T = "sizes";
    check_eq(T, "sample==RNET_INPUT_MAX", (unsigned)gba::kSioSampleBytes,
             (unsigned)RNET_INPUT_MAX);
    check_true(T, "version string", rnet_version_string() != nullptr);
}

void test_facade_start_shutdown() {
    const char* T = "facade_lan";
    gba::FrameLinkPartner link(0);
    gba_netplay_set_link_partner(&link);

    GbaNetplayConfig cfg{};
    gba_netplay_config_defaults(&cfg);
    cfg.enabled = 1;
    cfg.local_slot = 0;
    cfg.input_delay = 2;
    cfg.session_id = 0x0BA00001u;
    // Bind an ephemeral port; peer empty = host learns peer on first packet.
    std::snprintf(cfg.bind_hostport, sizeof(cfg.bind_hostport), "127.0.0.1:0");
    cfg.peer_hostport[0] = '\0';

    check_true(T, "start_lan", gba_netplay_start_lan(&cfg) != 0);
    check_true(T, "active", gba_netplay_active() != 0);
    check_eq(T, "slot", (unsigned)gba_netplay_local_slot(), 0u);

    gba_netplay_stage_keys(0x03FDu);
    link.begin_frame();
    gba_netplay_pump();
    // Alone: not running until peer handshake — admit must fail cleanly.
    check_eq(T, "admit while linking", (unsigned)gba_netplay_poll_admit(), 0u);

    gba_netplay_shutdown();
    check_eq(T, "inactive", (unsigned)gba_netplay_active(), 0u);
    gba_netplay_set_link_partner(nullptr);
}

void test_sample_via_partner() {
    const char* T = "sample_partner";
    gba::FrameLinkPartner link(0);
    link.set_force_cycles(8);
    gba::GbaIo io;
    io.set_link_partner(&link);

    link.begin_frame();
    io.write32(gba::IoReg::SIODATA32, 0xCAFEBABEu);
    io.write16(gba::IoReg::SIOCNT, 0x5081);
    io.tick_sio(8);

    uint8_t buf[gba::kSioSampleBytes]{};
    check_eq(T, "sample", (unsigned)link.sample(0x03FF, buf, sizeof(buf)),
             (unsigned)gba::kSioSampleBytes);
    gba::SioSampleView view{};
    check_true(T, "unpack", gba::sio_sample_unpack(buf, sizeof(buf), &view));
    check_eq(T, "count", view.count, 1u);
    check_eq(T, "tx", view.events[0].tx_data, 0xCAFEBABEu);
}

}  // namespace

int main() {
    test_sizes_align();
    test_sample_via_partner();
    test_facade_start_shutdown();

    if (failures) {
        std::printf("netplay_smoke_tests: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("netplay_smoke_tests: ok (recomp-net %s)\n",
                rnet_version_string());
    return 0;
}
