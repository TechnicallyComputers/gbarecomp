// gba_netplay.h — delay-sync facade over recomp-net for GBA link-cable hosts.
//
// Lockstep (see lib/recomp-net/docs/host_integration.md):
//   begin_frame → publish(remote N−D) happens inside poll_admit's publish cb
//   → host runs one frame / SIO transfers on FrameLinkPartner
//   → finish_frame (rnet_session_advance)
//
// Sample blob is gba::kSioSampleBytes (32) == RNET_INPUT_MAX: KEYINPUT +
// up to 3 SIO TX events (see gba_link.h).

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GbaNetplayConfig {
    int      enabled;
    int      local_slot;     /* 0 or 1 */
    int      input_delay;    /* frames; default 2 */
    uint32_t session_id;
    char     bind_hostport[64];
    char     peer_hostport[64];
} GbaNetplayConfig;

void gba_netplay_config_defaults(GbaNetplayConfig* cfg);

/* Attach the FrameLinkPartner already wired into GbaIo (not owned). */
#ifdef __cplusplus
namespace gba { class FrameLinkPartner; }
void gba_netplay_set_link_partner(gba::FrameLinkPartner* partner);
#else
void gba_netplay_set_link_partner(void* partner);
#endif

int  gba_netplay_active(void);
int  gba_netplay_is_running(void);
int  gba_netplay_local_slot(void);
uint32_t gba_netplay_sim_tick(void);

int  gba_netplay_start_lan(const GbaNetplayConfig* cfg);
void gba_netplay_shutdown(void);

/* Stage KEYINPUT for the next sample_local (active-low GBA bits). */
void gba_netplay_stage_keys(uint16_t keys);

void gba_netplay_pump(void);

/* Call FrameLinkPartner::begin_frame, then pump + try_admit.
 * On success, remote SIO events are already published into the partner. */
int  gba_netplay_poll_admit(void);

/* Call after one admitted sim frame. */
void gba_netplay_finish_frame(void);

int  gba_netplay_input_desync(uint32_t* tick, uint32_t* local_hash,
                              uint32_t* remote_hash);
int  gba_netplay_peer_disconnected(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
