# Local codegen SDK

Headless contract for regenerating an existing gbarecomp game project from a
user-supplied ROM. Intended for `recomp-ui` setup flows and RetComM launcher
automation. This does **not** redistribute ROM data or BIOS dumps.

## Setup host (CI without cart/BIOS generated C)

Games can ship a **setup host**: launcher + runtime linked **without** cart
shards (and with the tracked BIOS stub when retail BIOS C is absent). First-run
Generate emits cart C into `variants/<game>/generated/`, optionally regenerates
BIOS backends under `gbarecomp/src/runtime/generated_bios/`, then rebuilds.

Softening helpers:

| Option / env | Role |
|--------------|------|
| `GBARECOMP_ALLOW_NO_GENERATED=ON` | `runtime.cmake` skips fatal empty-shard checks |
| `GBARECOMP_FORCE_SETUP` (or game-specific) | force setup wizard in the host |
| BIOS stub already in-tree | runtime builds without `bios_recompiled.cpp` |

## Commands

```bash
python gbarecomp_cli.py verify-rom --rom GAME.gba \
  --expected-sha1 f3ae088181bf583e55daf962a92bb46f4f1d07b7 \
  [--expected-crc32 …] [--json-progress]

python gbarecomp_cli.py generate \
  --rom GAME.gba \
  --config variants/emerald/symbols/emerald_usa.toml \
  --out-dir variants/emerald/generated \
  --project-root /path/to/GameRecomp \
  [--bios path/to/gba_bios.bin] [--force-bios] \
  [--expected-sha1 …] \
  [--json-progress]

python gbarecomp_cli.py rebuild \
  --project-root /path/to/GameRecomp \
  --build-dir build \
  --target EmeraldRecomp \
  [--exe-basename EmeraldRecomp] \
  [--json-progress]
```

`build` remains the greenfield scaffolder (new empty project). `generate`
targets an existing title that already has reviewed per-binary TOML seeds.

`generate` drives `gba_recompile` (cart mode, and BIOS mode when a dump is
available / `--force-bios`). When `--expected-sha1` is omitted, the CLI reads
`[identity].sha1` from `--config` unless `--skip-hash-check` is set.

## Exit codes

| Code | Meaning |
|------|---------|
| 0 | success |
| 1 | runtime / generation / build failure |
| 2 | usage / argument error |
| 3 | ROM verification failure |

## JSONL progress (`--json-progress`)

Stdout is reserved for one JSON object per line. Useful events:

| `event` | Notes |
|---------|--------|
| `phase` | `phase`, optional `pct` / `message` (`verify`, `bios_emit`, `emit`, `build`, `done`) |
| `rom` | digests after verification |
| `log` | mirrored tool chatter |
| `result` | final payload (`ok`, `rom`, `out_dir`, …) |
| `error` | `message`, `code`, optional `details` |

Human-readable text goes to stderr when JSON progress is enabled.

## Portable recomp-ui host (`host/`)

Any game that embeds recomp-ui can reuse the full setup flow (pick ROM →
generate → cmake rebuild → relaunch) by compiling:

- `host/gbarecomp_codegen_host.c`
- `host/gbarecomp_codegen_host.h`

Put `recomp-ui/src` and `gbarecomp/host` on the include path, fill a
`GbarecompCodegenHostConfig`, then:

```c
gbarecomp_codegen_host_apply(&gi, &my_cfg);

/* after recomp_launcher_run_window: */
if (lr == RECOMP_LAUNCHER_RESULT_RELAUNCH)
    gbarecomp_codegen_host_relaunch_or_exit(rom_path);
```

### Config fields (minimum)

| Field | Example |
|-------|---------|
| `display_name` | `"Pokémon Emerald"` |
| `cmake_target` | `"EmeraldRecomp"` |
| `exe_basename` | `"EmeraldRecomp"` |
| `config` | `"variants/emerald/symbols/emerald_usa.toml"` |
| `out_dir` | `"variants/emerald/generated"` |
| `expected_sha1` | optional; else TOML `[identity]` |
| `seed_config_relpath` | same as `config` (project-root probe) |

Defaults cover `gbarecomp/gbarecomp_cli.py` and `build/`. Override with
`GBARECOMP_PROJECT_ROOT`, `GBARECOMP_BUILD_DIR`, `GBARECOMP_FORCE_SETUP`
(or per-game env names in the config). Point `GBARECOMP_RECOMPILE` at a
built `gba_recompile` binary when it is not on `PATH`.

### Platform rebuild behavior

| OS | Behavior |
|----|----------|
| Linux / macOS | In-process `cmake` reconfigure + `--build`, then `exec` the new binary |
| Windows | Writes `build/recomp_deferred_rebuild.cmd`, exits; helper waits for the game PID, builds, starts the new exe (avoids a locked `.exe`) |

`rebuild` always reconfigures (clears setup-host flags such as
`EMERALD_FORCE_SETUP_HOST`, clamps future zip mtimes for Ninja, and wipes
an incomplete cache that has `CMakeCache.txt` but no generator files).
Setup zips must ship `gbarecomp/third_party/tomlpp/toml.hpp` so configure
does not need network FetchContent.

Folder layout stays `build/` on every OS so RetComM and other tools can treat
projects uniformly.

### Game regen scripts

CLI wrappers should call `gbarecomp_cli.py generate` (same contract as the
host). Titles copy a thin config pattern (see Emerald `codegen_setup` once
wired).
