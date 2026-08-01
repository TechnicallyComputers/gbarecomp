#!/usr/bin/env python3
"""Headless ROM → generate / rebuild SDK for gbarecomp games.

Commands:
  verify-rom   Hash-check a .gba dump (SHA-1 primary)
  generate     Run gba_recompile into an existing game tree (+ optional BIOS)
  rebuild      cmake --build for an existing project
  build        Greenfield scaffolder (delegates to tools/cli.py)

Exit codes: 0 ok · 1 runtime · 2 usage · 3 ROM verify fail
"""

from __future__ import annotations

import argparse
import os
import pathlib
import sys


def resource_root() -> pathlib.Path:
    frozen = getattr(sys, "_MEIPASS", None)
    return pathlib.Path(frozen).resolve() if frozen else pathlib.Path(__file__).resolve().parent


ROOT = resource_root()
os.environ.setdefault("GBARECOMP_ROOT", str(ROOT))
for path in (ROOT, ROOT / "tools"):
    value = str(path)
    if value not in sys.path:
        sys.path.insert(0, value)

from sdk_generate import (  # noqa: E402
    EXIT_ERROR,
    EXIT_OK,
    EXIT_USAGE,
    add_generate_parser,
    add_rebuild_parser,
    add_verify_parser,
)
from sdk_progress import ProgressReporter  # noqa: E402


def build_command(args: argparse.Namespace, progress: ProgressReporter) -> int:
    """Greenfield scaffold — reuse tools/cli.py build semantics."""
    del progress  # human-oriented scaffold; no JSONL contract yet
    tools_dir = ROOT / "tools"
    if str(tools_dir) not in sys.path:
        sys.path.insert(0, str(tools_dir))
    import cli as greenfield  # noqa: E402

    ns = argparse.Namespace(
        rom=args.rom,
        output=args.output,
        config=args.config,
        symbols=args.symbols,
        entry=args.entry,
        max_functions=args.max_functions,
        codegen_shards=args.codegen_shards,
        force=args.force,
        verbose=args.verbose,
    )
    try:
        return int(greenfield.build(ns))
    except (FileNotFoundError, RuntimeError, ValueError) as exc:
        print(f"gbarecomp: error: {exc}", file=sys.stderr)
        return EXIT_USAGE


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog="gbarecomp_cli",
        description=(
            "Turn a GBA ROM into recompilation sources, or regenerate an "
            "existing project for UI / launcher automation."
        ),
    )
    sub = ap.add_subparsers(dest="command", required=True)

    build = sub.add_parser(
        "build",
        help="scaffold a new project and generate C from a ROM (greenfield)",
    )
    build.add_argument("--rom", required=True, help="path to a .gba ROM")
    build.add_argument("--output", "-o", required=True, help="new output directory")
    build.add_argument("--config", help="optional per-binary TOML")
    build.add_argument("--symbols", help="optional imported symbol TSV")
    build.add_argument("--entry", help="optional hexadecimal entry address")
    build.add_argument("--max-functions", type=int)
    build.add_argument("--codegen-shards", type=int, choices=range(2, 257))
    build.add_argument("--force", action="store_true")
    build.add_argument("--verbose", action="store_true")
    build.add_argument("--json-progress", action="store_true")
    build.set_defaults(handler=build_command)

    add_verify_parser(sub)
    add_generate_parser(sub)
    add_rebuild_parser(sub)
    return ap


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    progress = ProgressReporter(
        json_progress=bool(getattr(args, "json_progress", False))
    )
    try:
        return int(args.handler(args, progress))
    except BrokenPipeError:
        return EXIT_ERROR
    except KeyboardInterrupt:
        progress.error("interrupted", code=EXIT_ERROR)
        return EXIT_ERROR


if __name__ == "__main__":
    raise SystemExit(main())
