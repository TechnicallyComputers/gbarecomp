#!/usr/bin/env bash
# Package a redistributable gbarecomp tools tree for RetComM local builds.
# Does not include ROM dumps, BIOS dumps, or game-generated variants/*/generated.
#
# Usage:
#   scripts/package_gbarecomp_tools.sh <gbarecomp-root> [os-tag] [out-dir]
# Example:
#   scripts/package_gbarecomp_tools.sh . linux
#
# Writes: <out-dir>/gbarecomp-tools-<os-tag>-x64.zip

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GBA_ROOT="${1:-$ROOT}"
OS_TAG="${2:-linux}"
OUT="${3:-"$ROOT/dist/packs"}"

if [[ -z "$GBA_ROOT" || ! -f "$GBA_ROOT/gbarecomp_cli.py" ]]; then
  echo "usage: $0 <gbarecomp-root> [os-tag] [out-dir]" >&2
  echo "  gbarecomp-root must contain gbarecomp_cli.py" >&2
  exit 2
fi

GBA_ROOT="$(cd "$GBA_ROOT" && pwd)"
STAGE="$OUT/stage-gbarecomp-tools-$OS_TAG"
ZIP_NAME="gbarecomp-tools-${OS_TAG}-x64.zip"

rm -rf "$STAGE"
mkdir -p "$STAGE" "$OUT"

copy_tree() {
  local src="$1" dest="$2"
  if [[ -e "$src" ]]; then
    mkdir -p "$(dirname "$dest")"
    cp -a "$src" "$dest"
  fi
}

copy_tree "$GBA_ROOT/gbarecomp_cli.py" "$STAGE/gbarecomp_cli.py"
copy_tree "$GBA_ROOT/tools/sdk_progress.py" "$STAGE/tools/sdk_progress.py"
copy_tree "$GBA_ROOT/tools/sdk_rom.py" "$STAGE/tools/sdk_rom.py"
copy_tree "$GBA_ROOT/tools/sdk_generate.py" "$STAGE/tools/sdk_generate.py"
copy_tree "$GBA_ROOT/tools/cli.py" "$STAGE/tools/cli.py"
copy_tree "$GBA_ROOT/docs/LOCAL_CODEGEN_SDK.md" "$STAGE/docs/LOCAL_CODEGEN_SDK.md"
copy_tree "$GBA_ROOT/bios/gba_bios.toml" "$STAGE/bios/gba_bios.toml"
copy_tree "$GBA_ROOT/README.md" "$STAGE/README.md"

# Prefer a built emitter next to common build layouts.
for cand in \
  "$GBA_ROOT/build/gba_recompile" \
  "$GBA_ROOT/build/gba_recompile.exe" \
  "$GBA_ROOT/build/Release/gba_recompile.exe" \
  "$GBA_ROOT/gba_recompile" \
  "$GBA_ROOT/gba_recompile.exe"
do
  if [[ -f "$cand" ]]; then
    copy_tree "$cand" "$STAGE/$(basename "$cand")"
    break
  fi
done

find "$STAGE" -type d -name '__pycache__' -prune -exec rm -rf {} + 2>/dev/null || true
find "$STAGE" -type d -name '.git' -prune -exec rm -rf {} + 2>/dev/null || true

cat >"$STAGE/retcomm-sdk.json" <<'EOF'
{
  "cli": "gbarecomp_cli.py",
  "id": "gbarecomp-tools"
}
EOF

cat >"$STAGE/README.retcomm.md" <<EOF
# gbarecomp-tools ($OS_TAG)

Headless generate / verify-rom SDK for RetComM. Point the launcher at this
directory with \`RETCOMM_SDK_DIR\`, or publish as a GitHub release asset matching
catalog \`build.sdk.asset_glob\`.

Never ship user ROMs, BIOS dumps, or game \`variants/*/generated\` output in
this pack. Build \`gba_recompile\` into the pack root before zipping when
possible.
EOF

rm -f "$OUT/$ZIP_NAME"
( cd "$STAGE" && zip -qr "$OUT/$ZIP_NAME" . )
echo "Wrote $OUT/$ZIP_NAME"
echo "Smoke: RETCOMM_SDK_DIR=$STAGE python3 $STAGE/gbarecomp_cli.py --help"
