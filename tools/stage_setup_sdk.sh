#!/usr/bin/env bash
# Stage gbarecomp emitter + CLI marker + optional portable toolchain into an
# existing setup-host release stage directory.
#
# Title packagers copy the host exe, game sources, and framework tree first,
# then call this to finish the RetComM/wizard-complete zip layout.
#
# Usage:
#   stage_setup_sdk.sh --stage <stage-dir> [options]
#
# Options:
#   --framework DIR         gbarecomp source tree (default: <cwd>/gbarecomp)
#   --recompiler-build DIR  Where gba_recompile was built (repeatable)
#   --toolchain-dir DIR     Pack root with bin/; embedded as stage/toolchain/
#   --allow-no-toolchain    Warn instead of failing when toolchain unset
#   --host-exe PATH         Host PE for Windows DLL bundling (optional)
#   --runtime-bin DIR       MinGW runtime DLL search dir (repeatable)
#   --search-dir DIR        Extra DLL search dir (repeatable)
#   --require-cli           Require gbarecomp_cli.py (default on)
#
# Env aliases for toolchain dir (first wins):
#   GBARECOMP_TOOLCHAIN_DIR, EMERALD_TOOLCHAIN_DIR, TOOLCHAIN_DIR, BPE_TOOLCHAIN_DIR
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FW_TOOLS="${SCRIPT_DIR}"

STAGE=""
FRAMEWORK=""
RECOMPILER_BUILDS=()
TOOLCHAIN_DIR="${GBARECOMP_TOOLCHAIN_DIR:-${EMERALD_TOOLCHAIN_DIR:-${TOOLCHAIN_DIR:-${BPE_TOOLCHAIN_DIR:-}}}}"
ALLOW_NO_TOOLCHAIN=0
HOST_EXE=""
RUNTIME_BINS=()
SEARCH_DIRS=()
REQUIRE_CLI=1

usage() {
  sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'
  exit 2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage ;;
    --stage) STAGE="${2:?}"; shift 2 ;;
    --framework) FRAMEWORK="${2:?}"; shift 2 ;;
    --recompiler-build) RECOMPILER_BUILDS+=("${2:?}"); shift 2 ;;
    --toolchain-dir) TOOLCHAIN_DIR="${2:?}"; shift 2 ;;
    --allow-no-toolchain) ALLOW_NO_TOOLCHAIN=1; shift ;;
    --host-exe) HOST_EXE="${2:?}"; shift 2 ;;
    --runtime-bin) RUNTIME_BINS+=("${2:?}"); shift 2 ;;
    --search-dir) SEARCH_DIRS+=("${2:?}"); shift 2 ;;
    --require-cli) REQUIRE_CLI=1; shift ;;
    --no-require-cli) REQUIRE_CLI=0; shift ;;
    *)
      echo "error: unknown arg: $1" >&2
      usage
      ;;
  esac
done

if [[ -z "${STAGE}" ]]; then
  echo "error: --stage is required" >&2
  usage
fi
STAGE="$(cd "${STAGE}" && pwd)"

if [[ -z "${FRAMEWORK}" ]]; then
  if [[ -d "${PWD}/gbarecomp" ]]; then
    FRAMEWORK="$(cd "${PWD}/gbarecomp" && pwd)"
  else
    FRAMEWORK="$(cd "${SCRIPT_DIR}/.." && pwd)"
  fi
else
  FRAMEWORK="$(cd "${FRAMEWORK}" && pwd)"
fi

mkdir -p "${STAGE}/gbarecomp"

# Drop regenerated BIOS bodies / dumps if a prior copy left them.
rm -f "${STAGE}/gbarecomp/src/runtime/generated_bios/bios_recompiled.cpp" \
      "${STAGE}/gbarecomp/bios/gba_bios.bin" \
      "${STAGE}/gbarecomp/bios/"*.bin 2>/dev/null || true

find_tool_bin() {
  local name="$1"
  local dir cand
  local -a roots=()
  local r
  for r in "${RECOMPILER_BUILDS[@]+"${RECOMPILER_BUILDS[@]}"}"; do
    roots+=("$(cd "${r}" && pwd)")
  done
  roots+=(
    "${STAGE}/gbarecomp/build"
    "${FRAMEWORK}/build"
    "${PWD}/build-gba-tools"
    "${PWD}/build-ci/gbarecomp_build"
    "${PWD}/build-ci"
  )
  for dir in "${roots[@]}"; do
    [[ -d "${dir}" ]] || continue
    for cand in \
      "${dir}/${name}" \
      "${dir}/${name}.exe" \
      "${dir}/Release/${name}.exe"
    do
      if [[ -f "${cand}" ]]; then
        echo "${cand}"
        return 0
      fi
    done
  done
  return 1
}

GBA_BIN="$(find_tool_bin gba_recompile || true)"
if [[ -z "${GBA_BIN}" ]]; then
  echo "error: gba_recompile not found (pass --recompiler-build)" >&2
  exit 1
fi

GBA_BASENAME="$(basename "${GBA_BIN}")"
mkdir -p "${STAGE}/gbarecomp/build"
cp -a "${GBA_BIN}" "${STAGE}/gbarecomp/build/${GBA_BASENAME}"
cp -a "${GBA_BIN}" "${STAGE}/gbarecomp/${GBA_BASENAME}"
chmod +x "${STAGE}/gbarecomp/build/${GBA_BASENAME}" 2>/dev/null || true
chmod +x "${STAGE}/gbarecomp/${GBA_BASENAME}" 2>/dev/null || true
echo "staged emitter from ${GBA_BIN%/*}"

cat >"${STAGE}/gbarecomp/retcomm-sdk.json" <<'EOF'
{
  "cli": "gbarecomp_cli.py",
  "id": "gbarecomp-tools",
  "game_bin": "gba_recompile"
}
EOF

if [[ "${REQUIRE_CLI}" -eq 1 && ! -f "${STAGE}/gbarecomp/gbarecomp_cli.py" ]]; then
  echo "error: missing gbarecomp/gbarecomp_cli.py (copy framework into stage first)" >&2
  exit 1
fi

if [[ ! -f "${STAGE}/gbarecomp/bios/gba_bios.toml" ]]; then
  echo "error: missing gbarecomp/bios/gba_bios.toml in staged tree" >&2
  exit 1
fi

if [[ ! -f "${STAGE}/gbarecomp/third_party/tomlpp/toml.hpp" ]]; then
  echo "error: missing gbarecomp/third_party/tomlpp/toml.hpp (offline rebuild needs vendored toml++)" >&2
  exit 1
fi

if [[ -n "${TOOLCHAIN_DIR}" && -d "${TOOLCHAIN_DIR}" ]]; then
  if [[ ! -d "${TOOLCHAIN_DIR}/bin" ]]; then
    echo "error: toolchain dir missing bin/: ${TOOLCHAIN_DIR}" >&2
    exit 1
  fi
  mkdir -p "${STAGE}/toolchain"
  if command -v rsync >/dev/null 2>&1; then
    rsync -a --delete "${TOOLCHAIN_DIR}/" "${STAGE}/toolchain/"
  else
    rm -rf "${STAGE}/toolchain"
    mkdir -p "${STAGE}/toolchain"
    cp -a "${TOOLCHAIN_DIR}/." "${STAGE}/toolchain/"
  fi
  echo "bundled toolchain from ${TOOLCHAIN_DIR}"
elif [[ "${ALLOW_NO_TOOLCHAIN}" -eq 1 ]]; then
  echo "warning: toolchain unset — zip will need system cmake/ninja" >&2
else
  echo "error: toolchain dir required (pass --toolchain-dir or set GBARECOMP_TOOLCHAIN_DIR)" >&2
  exit 1
fi

BUNDLE="${FW_TOOLS}/bundle_mingw_dlls.sh"
if [[ ! -f "${BUNDLE}" && -f "${STAGE}/gbarecomp/tools/bundle_mingw_dlls.sh" ]]; then
  BUNDLE="${STAGE}/gbarecomp/tools/bundle_mingw_dlls.sh"
fi

need_dlls=0
if [[ -n "${HOST_EXE}" && "${HOST_EXE}" == *.exe ]]; then
  need_dlls=1
fi
if [[ "${GBA_BASENAME}" == *.exe ]]; then
  need_dlls=1
fi

if [[ "${need_dlls}" -eq 1 ]]; then
  if [[ ! -f "${BUNDLE}" ]]; then
    echo "error: bundle_mingw_dlls.sh not found next to stage_setup_sdk.sh" >&2
    exit 1
  fi
  chmod +x "${BUNDLE}" 2>/dev/null || true

  args=()
  for d in "${RUNTIME_BINS[@]+"${RUNTIME_BINS[@]}"}"; do
    args+=(--runtime-bin "${d}")
  done
  for d in "${SEARCH_DIRS[@]+"${SEARCH_DIRS[@]}"}"; do
    args+=(--search-dir "${d}")
  done
  if [[ -n "${HOST_EXE}" && -f "${HOST_EXE}" ]]; then
    args+=(
      --exe "${HOST_EXE}"
      --dest "${STAGE}"
      --label "$(basename "${HOST_EXE}")"
    )
  fi
  if [[ "${GBA_BASENAME}" == *.exe ]]; then
    args+=(
      --exe "${STAGE}/gbarecomp/build/${GBA_BASENAME}"
      --dest "${STAGE}/gbarecomp/build"
      --label "${GBA_BASENAME}"
      --exe "${STAGE}/gbarecomp/${GBA_BASENAME}"
      --dest "${STAGE}/gbarecomp"
      --label "${GBA_BASENAME}"
    )
  fi
  args+=(
    --require libgcc_s_seh-1.dll
    --require libstdc++-6.dll
  )
  bash "${BUNDLE}" "${args[@]}"
fi

echo "stage_setup_sdk: ready under ${STAGE}/gbarecomp"
