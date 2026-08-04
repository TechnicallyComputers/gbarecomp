/*
 * Engine-owned LAN/Direct-IP lobby adapter for recomp-ui (GBA link-cable).
 *
 * v1 is LAN-only — no MotK lobby server / ICE. Games call gba_host_lobby_init(),
 * then wire gba_host_lobby_callbacks() into RecompLauncherCGameInfo.netplay.
 * After LAUNCH, map settings.netplay_launch into GbaNetplayConfig (pending
 * or env) before run_game().
 *
 * Requires recomp-ui headers (RECOMP_LAUNCHER or GBARECOMP_HAS_RECOMP_UI).
 */
#ifndef GBA_HOST_LOBBY_H
#define GBA_HOST_LOBBY_H

#if defined(RECOMP_LAUNCHER) || defined(GBARECOMP_HAS_RECOMP_UI)
#include "recomp_launcher.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GbaHostLobbyIdentity {
    const char* game_name;         /* e.g. "Pokemon Emerald" */
    const char* game_version;      /* e.g. "0.1.0" */
    const char* lan_registry_path; /* e.g. "netplay_lan_lobby.txt" */
    const char* default_lobby_name;
} GbaHostLobbyIdentity;

/* Init once before first launcher open. Returns 0 on success. */
int gba_host_lobby_init(const GbaHostLobbyIdentity* id);
void gba_host_lobby_shutdown(void);

#if defined(RECOMP_LAUNCHER) || defined(GBARECOMP_HAS_RECOMP_UI)
const RecompLauncherCNetplayCallbacks* gba_host_lobby_callbacks(void);
#endif

int gba_host_lobby_leave(void);
void gba_host_lobby_disconnect(void);
int gba_host_lobby_in_lan(void);
const char* gba_host_lobby_resume_endpoint(void);
void gba_host_lobby_set_runtime_error(const char* error_code);

/* After a match ends: clear started, re-bind Direct listen, clear launch blob
 * so recomp-ui can reopen the waiting room (resume_netplay_room). */
void gba_host_lobby_prepare_rematch(void);

#ifdef __cplusplus
}
#endif

#endif /* GBA_HOST_LOBBY_H */
