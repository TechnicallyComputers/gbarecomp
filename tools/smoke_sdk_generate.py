"""Smoke-test headless ``verify-rom`` (and CLI wiring) without a retail ROM.

Full ``generate`` needs a built ``gba_recompile`` binary and a real dump; this
smoke covers the RetComM exit-code / JSONL contract for verify-rom.
"""

from __future__ import annotations

import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
import zlib


ROOT = pathlib.Path(__file__).resolve().parent.parent
CLI = ROOT / "gbarecomp_cli.py"


def write_fixture_rom(path: pathlib.Path) -> bytes:
    # Minimal shape accepted by sdk_rom (header-sized stub).
    rom = bytearray([0x00] * 512)
    rom[0:4] = b"\x00\x00\x00\xea"  # dummy ARM branch
    path.write_bytes(rom)
    return bytes(rom)


def run_cli(args: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(CLI), *args],
        cwd=str(ROOT),
        text=True,
        capture_output=True,
        check=False,
    )


def main() -> int:
    help_rc = run_cli(["--help"])
    if help_rc.returncode != 0:
        raise RuntimeError(f"--help failed: {help_rc.stderr}")

    with tempfile.TemporaryDirectory(prefix="gbarecomp-sdk-smoke-") as directory:
        root = pathlib.Path(directory)
        rom_path = root / "fixture.gba"
        raw = write_fixture_rom(rom_path)
        sha1 = hashlib.sha1(raw).hexdigest()
        crc32 = f"{zlib.crc32(raw) & 0xFFFFFFFF:08x}"

        verify = run_cli([
            "verify-rom",
            "--rom", str(rom_path),
            "--expected-sha1", sha1,
            "--expected-crc32", crc32,
            "--json-progress",
        ])
        if verify.returncode != 0:
            raise RuntimeError(
                f"verify-rom failed: rc={verify.returncode}\n"
                f"stdout:\n{verify.stdout}\nstderr:\n{verify.stderr}"
            )
        events = [
            json.loads(line) for line in verify.stdout.splitlines() if line.strip()
        ]
        if not any(event.get("event") == "result" and event.get("ok") for event in events):
            raise RuntimeError(f"verify-rom missing result event: {events}")

        bad = run_cli([
            "verify-rom",
            "--rom", str(rom_path),
            "--expected-sha1", "0" * 40,
        ])
        if bad.returncode != 3:
            raise RuntimeError(
                f"verify-rom should exit 3 on mismatch, got {bad.returncode}\n"
                f"stderr:\n{bad.stderr}"
            )

    print("gbarecomp SDK smoke ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
