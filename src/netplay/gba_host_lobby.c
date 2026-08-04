/* gba_host_lobby.c — LAN/Direct-IP waiting room for GBA recomp-ui netplay. */

#include "gba_host_lobby.h"

#include <stdio.h>
#include <string.h>

#include "recomp_net/address.h"
#include "recomp_net/lan_direct.h"
#include "recomp_net/lan_lobby.h"

#if !defined(RECOMP_LAUNCHER) && !defined(GBARECOMP_HAS_RECOMP_UI)

int gba_host_lobby_init(const GbaHostLobbyIdentity* id) {
    (void)id;
    return -1;
}
void gba_host_lobby_shutdown(void) {}
int gba_host_lobby_leave(void) { return -1; }
void gba_host_lobby_disconnect(void) {}
int gba_host_lobby_in_lan(void) { return 0; }
const char* gba_host_lobby_resume_endpoint(void) { return ""; }
void gba_host_lobby_set_runtime_error(const char* error_code) {
    (void)error_code;
}
void gba_host_lobby_prepare_rematch(void) {}

#else

enum { kMaxLocalAddresses = 16 };

static GbaHostLobbyIdentity g_id;
static int g_inited;
static int g_hosting_lan;
static int g_joined_lan;
static int g_joined_direct;
static RecompLauncherCNetplayLaunch g_lan_launch;
static RNetLanLobby g_lan_room;
static RNetLanDirectHost* g_direct_host;
static RNetLanDirectGuest* g_direct_guest;
static char g_direct_peer_endpoint[64];
static char g_resume_endpoint[64];
static char g_runtime_error[64];
static char g_player_name[64];
static char g_last_error[64];
static RNetIpv4Address g_local_addresses[kMaxLocalAddresses];
static int g_local_address_count;
static char g_external_ip[RNET_IPV4_ADDRESS_TEXT_MAX];
static int g_lobby_input_delay = 2;
static int g_lan_guest_rtt_ms = -1;

static int clamp_input_delay(int delay) {
    if (delay < 2) return 2;
    if (delay > 20) return 20;
    return delay;
}

static const char* lan_path(void) {
    return g_id.lan_registry_path && g_id.lan_registry_path[0]
               ? g_id.lan_registry_path
               : "netplay_lan_lobby.txt";
}

static const char* game_name(void) {
    return g_id.game_name && g_id.game_name[0] ? g_id.game_name
                                               : "GBA Game";
}

static const char* game_version(void) {
    return g_id.game_version && g_id.game_version[0] ? g_id.game_version
                                                    : "0.0.0";
}

static const char* display_name(void) {
    return g_player_name[0] ? g_player_name : "Player";
}

static void close_direct_sockets(void) {
    rnet_lan_direct_host_close(&g_direct_host);
    rnet_lan_direct_guest_close(&g_direct_guest);
}

static int publish_lan_room(void) {
    return rnet_lan_lobby_publish(lan_path(), &g_lan_room) == RNET_LAN_LOBBY_OK;
}

static int read_lan(RNetLanLobby* state) {
    return rnet_lan_lobby_read(lan_path(), game_name(), game_version(),
                               state) == RNET_LAN_LOBBY_OK;
}

static void set_err(const char* code) {
    snprintf(g_last_error, sizeof(g_last_error), "%s", code ? code : "");
}

static int create_lan(const char* name, const char* endpoint,
                      const char* password) {
    char advertised[RNET_LAN_LOBBY_ENDPOINT_MAX];
    const char* stored_endpoint = endpoint;
    const char* colon;
    const char* port;
    char bind_hp[64];

    memset(&g_lan_room, 0, sizeof(g_lan_room));
    close_direct_sockets();
    snprintf(g_lan_room.name, sizeof(g_lan_room.name), "%s",
             name && name[0]
                 ? name
                 : (g_id.default_lobby_name ? g_id.default_lobby_name
                                            : "LAN Lobby"));
    snprintf(g_lan_room.game, sizeof(g_lan_room.game), "%s", game_name());
    snprintf(g_lan_room.game_version, sizeof(g_lan_room.game_version), "%s",
             game_version());
    if (endpoint && strncmp(endpoint, "0.0.0.0:", 8) == 0) {
        RNetIpv4Address address;
        if (rnet_ipv4_enumerate(&address, 1) > 0 && address.address[0]) {
            snprintf(advertised, sizeof(advertised), "%s:%s", address.address,
                     endpoint + 8);
            stored_endpoint = advertised;
        }
    }
    snprintf(g_lan_room.endpoint, sizeof(g_lan_room.endpoint), "%s",
             stored_endpoint && stored_endpoint[0] ? stored_endpoint
                                                   : "127.0.0.1:7777");
    snprintf(g_lan_room.host_name, sizeof(g_lan_room.host_name), "%s",
             display_name());
    snprintf(g_lan_room.password, sizeof(g_lan_room.password), "%s",
             password ? password : "");
    g_lan_room.host_slot = 0;
    g_lan_room.input_delay = clamp_input_delay(g_lobby_input_delay);
    if (!publish_lan_room()) {
        set_err("lan_publish_failed");
        return 0;
    }
    colon = strrchr(g_lan_room.endpoint, ':');
    port = colon ? colon + 1 : "7777";
    snprintf(bind_hp, sizeof(bind_hp), "0.0.0.0:%s", port);
    if (rnet_lan_direct_host_open(&g_direct_host, bind_hp, &g_lan_room) !=
        RNET_LAN_DIRECT_OK) {
        fprintf(stderr,
                "gba_host_lobby: Direct IP listen failed on %s — same-machine "
                "file join still works; remote Join Direct will time out\n",
                bind_hp);
    }
    g_hosting_lan = 1;
    g_joined_lan = 0;
    g_joined_direct = 0;
    g_direct_peer_endpoint[0] = '\0';
    g_lan_guest_rtt_ms = -1;
    memset(&g_lan_launch, 0, sizeof(g_lan_launch));
    snprintf(g_resume_endpoint, sizeof(g_resume_endpoint), "%s",
             g_lan_room.endpoint);
    g_last_error[0] = '\0';
    return 1;
}

static int fill_lan_row(RecompLauncherCNetplayLobby* out) {
    RNetLanLobby state;
    if (!out) return 0;
    if (g_hosting_lan)
        state = g_lan_room;
    else if (!read_lan(&state))
        return 0;
    memset(out, 0, sizeof(*out));
    snprintf(out->lobby_id, sizeof(out->lobby_id), "lan:%s", state.endpoint);
    snprintf(out->name, sizeof(out->name), "LAN - %s",
             state.name[0] ? state.name : "Lobby");
    snprintf(out->game_name, sizeof(out->game_name), "%s", state.game);
    snprintf(out->game_version, sizeof(out->game_version), "%s",
             state.game_version);
    out->player_count = state.joiner_name[0] ? 2 : 1;
    out->max_slots = 2;
    out->has_password = state.password[0] != '\0';
    out->latency_ms = -1;
    return 1;
}

static void clear_lan_joiner(void) {
    if (g_joined_direct && g_direct_guest)
        (void)rnet_lan_direct_guest_leave(g_direct_guest);
    close_direct_sockets();
    g_joined_lan = 0;
    g_joined_direct = 0;
    g_direct_peer_endpoint[0] = '\0';
    g_lan_guest_rtt_ms = -1;
    memset(&g_lan_room, 0, sizeof(g_lan_room));
    memset(&g_lan_launch, 0, sizeof(g_lan_launch));
}

static void sync_lan_joiner(void) {
    RNetLanLobby state;
    const char* name;
    int ev;
    if (!g_joined_lan) return;
    if (g_joined_direct) {
        int rtt = -1;
        ev = rnet_lan_direct_guest_pump(g_direct_guest, &g_lan_room, &rtt);
        if (ev == 2)
            clear_lan_joiner();
        else if (ev == 3 && rtt >= 0)
            g_lan_guest_rtt_ms = rtt;
        return;
    }
    if (!read_lan(&state)) {
        clear_lan_joiner();
        return;
    }
    name = display_name();
    if (!state.joiner_name[0] ||
        (name && name[0] && strcmp(state.joiner_name, name) != 0))
        clear_lan_joiner();
}

/* Same-machine guests claim the seat via the file registry without a UDP
 * JOIN_REQ. Host membership is otherwise only updated by direct_host_pump —
 * merge the registry joiner into g_lan_room so the room UI sees P2. */
static void sync_host_from_registry(void) {
    RNetLanLobby file;
    if (!g_hosting_lan) return;
    if (!read_lan(&file)) return;
    if (g_lan_room.endpoint[0] && file.endpoint[0] &&
        strcmp(g_lan_room.endpoint, file.endpoint) != 0)
        return;
    if (strcmp(g_lan_room.joiner_name, file.joiner_name) == 0) return;
    snprintf(g_lan_room.joiner_name, sizeof(g_lan_room.joiner_name), "%s",
             file.joiner_name);
    if (!file.joiner_name[0]) g_lan_guest_rtt_ms = -1;
}

static int use_lan_members(RNetLanLobby* state) {
    RNetLanLobby local;
    if (!state) state = &local;
    sync_lan_joiner();
    if (g_hosting_lan || g_joined_direct) {
        *state = g_lan_room;
        return g_hosting_lan || g_joined_lan;
    }
    if (!read_lan(state)) return 0;
    if (g_joined_lan) return 1;
    return 0;
}

static void arm_lan_launch(const RNetLanLobby* state) {
    const char* colon;
    const char* port;
    const char* peer;
    if (!state) return;
    if (g_hosting_lan)
        (void)rnet_lan_direct_host_notify_start(g_direct_host, state);
    close_direct_sockets();
    memset(&g_lan_launch, 0, sizeof(g_lan_launch));
    g_lan_launch.enabled = 1;
    g_lan_launch.local_slot =
        g_hosting_lan ? state->host_slot : 1 - state->host_slot;
    g_lan_launch.input_player = 0;
    g_lan_launch.session_id = 1;
    g_lan_launch.input_delay =
        clamp_input_delay(state->input_delay >= 2 ? state->input_delay
                                                  : g_lobby_input_delay);
    g_lan_launch.max_slots = 2;
    g_lan_launch.player_count = 2;
    if (g_hosting_lan) {
        colon = strrchr(state->endpoint, ':');
        port = colon ? colon + 1 : "7777";
        snprintf(g_lan_launch.bind_hostport, sizeof(g_lan_launch.bind_hostport),
                 "0.0.0.0:%s", port);
    } else {
        peer = g_direct_peer_endpoint[0] ? g_direct_peer_endpoint
                                         : state->endpoint;
        snprintf(g_lan_launch.bind_hostport, sizeof(g_lan_launch.bind_hostport),
                 "0.0.0.0:0");
        snprintf(g_lan_launch.peer_hostport, sizeof(g_lan_launch.peer_hostport),
                 "%s", peer);
    }
}

int gba_host_lobby_init(const GbaHostLobbyIdentity* id) {
    if (!id || !id->game_name || !id->game_name[0]) return -1;
    memset(&g_id, 0, sizeof(g_id));
    g_id = *id;
    g_hosting_lan = 0;
    g_joined_lan = 0;
    g_joined_direct = 0;
    close_direct_sockets();
    memset(&g_lan_room, 0, sizeof(g_lan_room));
    memset(&g_lan_launch, 0, sizeof(g_lan_launch));
    g_direct_peer_endpoint[0] = '\0';
    g_resume_endpoint[0] = '\0';
    g_runtime_error[0] = '\0';
    g_last_error[0] = '\0';
    g_external_ip[0] = '\0';
    g_local_address_count = 0;
    if (!g_player_name[0])
        snprintf(g_player_name, sizeof(g_player_name), "%s", "Player");
    g_inited = 1;
    return 0;
}

void gba_host_lobby_shutdown(void) {
    gba_host_lobby_disconnect();
    g_inited = 0;
}

int gba_host_lobby_leave(void) {
    if (g_hosting_lan) {
        (void)rnet_lan_direct_host_notify_close(g_direct_host);
        close_direct_sockets();
        (void)rnet_lan_lobby_leave(lan_path(), 1);
    } else if (g_joined_direct) {
        if (g_direct_guest)
            (void)rnet_lan_direct_guest_leave(g_direct_guest);
        close_direct_sockets();
    } else if (g_joined_lan) {
        (void)rnet_lan_lobby_leave(lan_path(), 0);
    }
    g_hosting_lan = 0;
    g_joined_lan = 0;
    g_joined_direct = 0;
    g_direct_peer_endpoint[0] = '\0';
    memset(&g_lan_room, 0, sizeof(g_lan_room));
    memset(&g_lan_launch, 0, sizeof(g_lan_launch));
    return 0;
}

void gba_host_lobby_disconnect(void) { (void)gba_host_lobby_leave(); }

const char* gba_host_lobby_resume_endpoint(void) {
    return g_resume_endpoint[0] ? g_resume_endpoint : "";
}

int gba_host_lobby_in_lan(void) { return g_hosting_lan || g_joined_lan; }

void gba_host_lobby_set_runtime_error(const char* error_code) {
    snprintf(g_runtime_error, sizeof(g_runtime_error), "%s",
             error_code ? error_code : "");
}

void gba_host_lobby_prepare_rematch(void) {
    const char* colon;
    const char* port;
    char bind_hp[64];
    if (g_hosting_lan || g_joined_lan) {
        g_lan_room.started = 0;
        (void)rnet_lan_lobby_set_started(lan_path(), 0);
    }
    /* Game session released the UDP port — re-open Direct waiting-room listen. */
    if (g_hosting_lan && !g_direct_host && g_lan_room.endpoint[0]) {
        colon = strrchr(g_lan_room.endpoint, ':');
        port = colon ? colon + 1 : "7777";
        snprintf(bind_hp, sizeof(bind_hp), "0.0.0.0:%s", port);
        (void)rnet_lan_direct_host_open(&g_direct_host, bind_hp, &g_lan_room);
    }
    memset(&g_lan_launch, 0, sizeof(g_lan_launch));
    g_last_error[0] = '\0';
    g_runtime_error[0] = '\0';
}

static const char* cb_default_url(void* ctx) {
    (void)ctx;
    return "";
}

static void cb_set_url(void* ctx, const char* url) {
    (void)ctx;
    (void)url;
}

static int cb_connect(void* ctx) {
    (void)ctx;
    /* LAN-only adapter: MotK/WS is not used. UI still calls connect when
     * opening Host Lobby — succeed quietly so status is not "online_unsupported". */
    g_last_error[0] = '\0';
    return 0;
}

static int cb_connected(void* ctx) {
    (void)ctx;
    /* Report "connected" so online-gated UI paths do not block after no-op connect.
     * Online create() still fails with online_unsupported. */
    return 1;
}

static void cb_pump(void* ctx) {
    (void)ctx;
    if (g_hosting_lan) {
        if (g_direct_host) {
            int rtt = -1;
            if (rnet_lan_direct_host_pump(g_direct_host, &g_lan_room, &rtt))
                (void)publish_lan_room();
            if (rtt >= 0) g_lan_guest_rtt_ms = rtt;
            {
                static unsigned s_tick;
                s_tick++;
                if ((s_tick % 60) == 0)
                    (void)rnet_lan_direct_host_ping(g_direct_host);
            }
        }
        sync_host_from_registry();
    }
    if (g_joined_direct && g_direct_guest) {
        static unsigned s_gtick;
        s_gtick++;
        if ((s_gtick % 60) == 0)
            (void)rnet_lan_direct_guest_ping(g_direct_guest);
    }
    sync_lan_joiner();
}

static void cb_set_player_name(void* ctx, const char* name) {
    (void)ctx;
    snprintf(g_player_name, sizeof(g_player_name), "%s",
             name && name[0] ? name : "Player");
}

static const char* cb_player_name(void* ctx) {
    (void)ctx;
    return display_name();
}

static void cb_request_list(void* ctx) { (void)ctx; }

static int cb_list_count(void* ctx) {
    RecompLauncherCNetplayLobby lan;
    (void)ctx;
    return fill_lan_row(&lan) ? 1 : 0;
}

static int cb_list_get(void* ctx, int index, RecompLauncherCNetplayLobby* out) {
    (void)ctx;
    if (!out || index != 0) return 0;
    return fill_lan_row(out);
}

static int refresh_local_addresses(void) {
    int count = rnet_ipv4_enumerate(g_local_addresses, kMaxLocalAddresses);
    if (count < 0) count = 0;
    if (count > kMaxLocalAddresses) count = kMaxLocalAddresses;
    g_local_address_count = count;
    return count;
}

static int cb_local_address_get(void* ctx, int index,
                                RecompLauncherCNetplayLocalAddress* out) {
    (void)ctx;
    if (!out || index < 0) return 0;
    if (index == 0) refresh_local_addresses();
    if (index >= g_local_address_count) return 0;
    memset(out, 0, sizeof(*out));
    snprintf(out->address, sizeof(out->address), "%s",
             g_local_addresses[index].address);
    snprintf(out->label, sizeof(out->label), "%s",
             g_local_addresses[index].interface_label);
    return 1;
}

static int cb_local_ip(void* ctx, char* out, size_t out_len) {
    RecompLauncherCNetplayLocalAddress address;
    if (!out || !out_len || !cb_local_address_get(ctx, 0, &address)) return 0;
    snprintf(out, out_len, "%s", address.address);
    return out[0] != '\0';
}

static int cb_external_ip(void* ctx, char* out, size_t out_len) {
    RNetExternalIpv4Config config;
    int rc;
    (void)ctx;
    if (!out || !out_len) return 0;
    if (!g_external_ip[0]) {
        rnet_external_ipv4_config_init(&config);
        config.timeout_ms = 900;
        rc = rnet_external_ipv4_discover(&config, g_external_ip,
                                         sizeof(g_external_ip));
        if (rc != RNET_EXTERNAL_IPV4_OK) {
            snprintf(out, out_len, "Unavailable");
            return 0;
        }
    }
    snprintf(out, out_len, "%s", g_external_ip);
    return out[0] != '\0';
}

static int cb_create(void* ctx, const char* lobby_name, char* host_endpoint,
                     const char* password,
                     const RecompLauncherCSettings* settings, int lan_only,
                     int max_slots) {
    (void)ctx;
    (void)settings;
    (void)max_slots;
    if (!host_endpoint) return -1;
    if (!lan_only) {
        set_err("online_unsupported");
        return -1;
    }
    if (!host_endpoint[0])
        snprintf(host_endpoint, 64, "127.0.0.1:7777");
    if (!create_lan(lobby_name, host_endpoint, password)) return -1;
    return 0;
}

static int map_direct_join_rc(int rc) {
    if (rc == RNET_LAN_DIRECT_OK) return 0;
    if (rc == RNET_LAN_DIRECT_ERR_PASSWORD || rc == RNET_LAN_LOBBY_ERR_PASSWORD)
        return -2;
    if (rc == RNET_LAN_DIRECT_ERR_TIMEOUT || rc == RNET_LAN_DIRECT_ERR_IO ||
        rc == RNET_LAN_LOBBY_ERR_IO)
        return -3;
    return -1;
}

static int cb_join(void* ctx, const char* lobby_id, const char* password,
                   char* guest_bind) {
    RNetLanLobby state;
    const char* name;
    const char* endpoint;
    int rc;
    (void)ctx;
    memset(&g_lan_launch, 0, sizeof(g_lan_launch));
    if (!lobby_id || strncmp(lobby_id, "lan:", 4) != 0) {
        set_err("online_unsupported");
        return -1;
    }
    name = display_name();
    endpoint = lobby_id + 4;
    close_direct_sockets();
    g_hosting_lan = 0;
    g_joined_lan = 0;
    g_joined_direct = 0;
    g_direct_peer_endpoint[0] = '\0';

    rc = rnet_lan_lobby_join(lan_path(), game_name(), game_version(),
                             password ? password : "", name, &state);
    if (rc == RNET_LAN_LOBBY_OK) {
        g_lan_room = state;
        g_joined_lan = 1;
        snprintf(g_resume_endpoint, sizeof(g_resume_endpoint), "%s",
                 state.endpoint);
        snprintf(g_direct_peer_endpoint, sizeof(g_direct_peer_endpoint), "%s",
                 endpoint[0] ? endpoint : state.endpoint);
        g_last_error[0] = '\0';
        return 0;
    }

    rc = rnet_lan_direct_guest_join(endpoint, game_name(), game_version(),
                                    password ? password : "", name, guest_bind,
                                    2500, &state, &g_direct_guest);
    if (rc != RNET_LAN_DIRECT_OK) return map_direct_join_rc(rc);
    g_lan_room = state;
    g_joined_lan = 1;
    g_joined_direct = 1;
    snprintf(g_direct_peer_endpoint, sizeof(g_direct_peer_endpoint), "%s",
             endpoint);
    snprintf(g_resume_endpoint, sizeof(g_resume_endpoint), "%s", endpoint);
    g_last_error[0] = '\0';
    return 0;
}

static int cb_leave(void* ctx) {
    (void)ctx;
    return gba_host_lobby_leave();
}

static int cb_in_lobby(void* ctx) {
    (void)ctx;
    sync_lan_joiner();
    return g_hosting_lan || g_joined_lan;
}

static int cb_is_host(void* ctx) {
    (void)ctx;
    return g_hosting_lan ? 1 : 0;
}

static int cb_member_count(void* ctx) {
    RNetLanLobby state;
    (void)ctx;
    return use_lan_members(&state) ? 2 : 0;
}

static int cb_member_get(void* ctx, int index,
                         RecompLauncherCNetplayMember* out) {
    RNetLanLobby state;
    (void)ctx;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    out->latency_ms = -1;
    if (!use_lan_members(&state)) return 0;
    if (index < 0 || index > 1) return 0;
    out->slot = index == 0 ? state.host_slot : 1 - state.host_slot;
    out->ready = index == 0 || state.joiner_name[0] != '\0';
    out->is_host = index == 0;
    snprintf(out->display_name, sizeof(out->display_name), "%s",
             index == 0 ? state.host_name : state.joiner_name);
    if (g_hosting_lan)
        out->is_local = index == 0;
    else if (g_joined_lan)
        out->is_local = index == 1;
    if (!out->is_host && out->display_name[0] && g_lan_guest_rtt_ms >= 0)
        out->latency_ms = g_lan_guest_rtt_ms;
    return 1;
}

static int cb_move_member(void* ctx, int from_slot, int to_slot) {
    (void)ctx;
    if (g_hosting_lan && from_slot >= 0 && from_slot <= 1 && to_slot >= 0 &&
        to_slot <= 1 && from_slot != to_slot) {
        g_lan_room.host_slot = 1 - g_lan_room.host_slot;
        g_lan_room.started = 0;
        (void)publish_lan_room();
        return 0;
    }
    return -1;
}

static int cb_kick_member(void* ctx, int slot) {
    int guest_slot;
    (void)ctx;
    if (!g_hosting_lan) return -1;
    guest_slot = 1 - g_lan_room.host_slot;
    if (slot < 0 || slot > 1 || slot != guest_slot || !g_lan_room.joiner_name[0])
        return -1;
    (void)rnet_lan_direct_host_notify_kick(g_direct_host);
    g_lan_room.joiner_name[0] = '\0';
    g_lan_room.started = 0;
    (void)publish_lan_room();
    return 0;
}

static int cb_local_ready(void* ctx) {
    (void)ctx;
    return (g_hosting_lan || g_joined_lan) ? 1 : 0;
}

static int cb_all_ready(void* ctx) {
    RNetLanLobby state;
    (void)ctx;
    if (use_lan_members(&state)) return state.joiner_name[0] != '\0';
    return 0;
}

static int cb_set_ready(void* ctx, int ready) {
    (void)ctx;
    (void)ready;
    return 0;
}

static int cb_request_start(void* ctx, const RecompLauncherCSettings* settings) {
    (void)ctx;
    (void)settings;
    if (!g_hosting_lan) {
        set_err("not_host");
        return -1;
    }
    if (!g_lan_room.joiner_name[0]) {
        set_err("need_players");
        return -1;
    }
    g_lan_room.started = 1;
    g_lan_room.input_delay = clamp_input_delay(g_lobby_input_delay);
    (void)publish_lan_room();
    arm_lan_launch(&g_lan_room);
    g_last_error[0] = '\0';
    return 0;
}

static int cb_launch_pending(void* ctx) {
    RNetLanLobby state;
    int ev;
    (void)ctx;
    if (g_joined_direct && !g_lan_launch.enabled) {
        int rtt = -1;
        ev = rnet_lan_direct_guest_pump(g_direct_guest, &g_lan_room, &rtt);
        if (ev == 3 && rtt >= 0) g_lan_guest_rtt_ms = rtt;
        if (ev == 1 || g_lan_room.started) arm_lan_launch(&g_lan_room);
    } else if (g_joined_lan && !g_joined_direct && !g_lan_launch.enabled &&
               read_lan(&state) && state.started) {
        g_lan_room = state;
        arm_lan_launch(&g_lan_room);
    } else if (g_hosting_lan && !g_lan_launch.enabled && g_lan_room.started) {
        arm_lan_launch(&g_lan_room);
    }
    return g_lan_launch.enabled;
}

static void cb_clear_launch_pending(void* ctx) {
    (void)ctx;
    memset(&g_lan_launch, 0, sizeof(g_lan_launch));
}

static int cb_fill_launch(void* ctx, RecompLauncherCNetplayLaunch* out) {
    (void)ctx;
    if (!out || !g_lan_launch.enabled) return 0;
    *out = g_lan_launch;
    out->force_input_relay = 0;
    out->force_turn = 0;
    out->max_slots = 2;
    out->player_count = 2;
    return 1;
}

static const char* cb_last_error(void* ctx) {
    (void)ctx;
    if (g_runtime_error[0]) return g_runtime_error;
    return g_last_error;
}

static void cb_clear_last_error(void* ctx) {
    (void)ctx;
    g_runtime_error[0] = '\0';
    g_last_error[0] = '\0';
}

static int cb_input_delay_get(void* ctx) {
    RNetLanLobby state;
    (void)ctx;
    if (g_hosting_lan || g_joined_lan) {
        if (use_lan_members(&state) && state.input_delay >= 2)
            return clamp_input_delay(state.input_delay);
        return clamp_input_delay(g_lobby_input_delay);
    }
    return clamp_input_delay(g_lobby_input_delay);
}

static int cb_input_delay_set(void* ctx, int delay_frames) {
    (void)ctx;
    g_lobby_input_delay = clamp_input_delay(delay_frames);
    if (g_hosting_lan) {
        g_lan_room.input_delay = g_lobby_input_delay;
        (void)publish_lan_room();
        if (g_direct_host && g_lan_room.joiner_name[0])
            (void)rnet_lan_direct_host_notify_caps(g_direct_host,
                                                   g_lobby_input_delay);
    }
    return 0;
}

static int cb_force_input_relay_get(void* ctx) {
    (void)ctx;
    return 0;
}

static int cb_force_input_relay_set(void* ctx, int force) {
    (void)ctx;
    (void)force;
    return 0;
}

static int cb_lobby_max_slots(void* ctx) {
    (void)ctx;
    if (g_hosting_lan || g_joined_lan || g_joined_direct) return 2;
    return 0;
}

static int cb_force_turn_get(void* ctx) {
    (void)ctx;
    return 0;
}

static int cb_force_turn_set(void* ctx, int force) {
    (void)ctx;
    (void)force;
    return 0;
}

static RecompLauncherCNetplayCallbacks g_callbacks = {
    NULL,
    cb_default_url,
    cb_set_url,
    cb_connect,
    cb_connected,
    cb_pump,
    cb_set_player_name,
    cb_player_name,
    cb_request_list,
    cb_list_count,
    cb_list_get,
    cb_local_ip,
    cb_external_ip,
    cb_create,
    cb_join,
    cb_leave,
    cb_in_lobby,
    cb_is_host,
    cb_member_count,
    cb_member_get,
    cb_move_member,
    cb_local_ready,
    cb_all_ready,
    cb_set_ready,
    cb_request_start,
    cb_launch_pending,
    cb_clear_launch_pending,
    cb_fill_launch,
    cb_local_address_get,
    cb_kick_member,
    cb_last_error,
    cb_clear_last_error,
    cb_input_delay_get,
    cb_input_delay_set,
    cb_force_input_relay_get,
    cb_force_input_relay_set,
    cb_lobby_max_slots,
    cb_force_turn_get,
    cb_force_turn_set,
};

const RecompLauncherCNetplayCallbacks* gba_host_lobby_callbacks(void) {
    return g_inited ? &g_callbacks : NULL;
}

#endif /* RECOMP_LAUNCHER || GBARECOMP_HAS_RECOMP_UI */
