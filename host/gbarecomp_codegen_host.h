/* Portable recomp-ui setup host: ROM → generate → cmake rebuild → relaunch.
 *
 * Games compile gbarecomp_codegen_host.c, fill GbarecompCodegenHostConfig,
 * and call gbarecomp_codegen_host_apply() when building RecompLauncherCGameInfo.
 *
 * Requires recomp_launcher.h on the include path (recomp-ui submodule).
 */
#ifndef GBARECOMP_CODEGEN_HOST_H
#define GBARECOMP_CODEGEN_HOST_H

#include "recomp_launcher.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GbarecompCodegenHostConfig {
    /* Display name for notes / Windows helper console title. */
    const char* display_name;

    /* Env vars (optional). Defaults: GBARECOMP_PROJECT_ROOT / GBARECOMP_BUILD_DIR /
     * GBARECOMP_FORCE_SETUP when NULL. */
    const char* project_root_env;
    const char* build_dir_env;
    const char* force_setup_env;

    /* Paths relative to project root. NULL → gbarecomp defaults below. */
    const char* gbarecomp_cli_relpath; /* default: gbarecomp/gbarecomp_cli.py */
    const char* seed_config_relpath;   /* root probe; e.g. variants/emerald/symbols/….toml */
    const char* config;                /* --config path (required for generate) */
    const char* out_dir;               /* --out-dir; e.g. variants/emerald/generated */
    const char* gen_marker_relpath;    /* default: <out_dir>/dispatch_table.cpp */
    const char* bios_relpath;          /* optional default BIOS dump for --bios */
    const char* build_dir_name;        /* default: build */

    /* CMake / binary identity (required for auto-rebuild). */
    const char* cmake_target;          /* e.g. EmeraldRecomp */
    const char* exe_basename;          /* no .exe; e.g. EmeraldRecomp */

    /* Optional ROM digest for generate --expected-sha1 (NULL = use TOML identity). */
    const char* expected_sha1;

    /* Optional UI copy overrides (NULL → generic defaults). */
    const char* prepare_note;
    const char* prepare_note_windows;
    const char* prepare_note_no_cmake;
} GbarecompCodegenHostConfig;

/* Wire prepare/rebuild/relaunch callbacks onto gi when tools are discoverable. */
void gbarecomp_codegen_host_apply(RecompLauncherCGameInfo* gi,
                                  const GbarecompCodegenHostConfig* cfg);

/* True when gen_marker is missing under the discovered project root. */
int gbarecomp_codegen_host_sources_missing(
    const GbarecompCodegenHostConfig* cfg);

/* After run_window returns RECOMP_LAUNCHER_RESULT_RELAUNCH. Does not return
 * on success (exec / spawn helper + exit). */
void gbarecomp_codegen_host_relaunch_or_exit(const char* rom_path);

#ifdef __cplusplus
}
#endif

#endif /* GBARECOMP_CODEGEN_HOST_H */
