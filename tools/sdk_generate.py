"""Headless ROM → C generate orchestration for UIs and launchers.

Stable entry points:
  python gbarecomp_cli.py generate ...
  python gbarecomp_cli.py verify-rom ...

Exit codes:
  0  success
  1  runtime / generation failure
  2  usage / argument error
  3  ROM verification failure
"""

from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import time
from typing import Any, Optional

from sdk_progress import ProgressReporter
from sdk_rom import RomVerifyError, verify_rom

try:
    from toolchain_pack import (  # type: ignore
        activate_toolchain_bin,
        ensure_toolchain as ensure_toolchain_pack,
        resolve_toolchain_bin as resolve_pack_toolchain_bin,
    )
except ImportError:  # pragma: no cover
    activate_toolchain_bin = None  # type: ignore
    ensure_toolchain_pack = None  # type: ignore
    resolve_pack_toolchain_bin = None  # type: ignore


EXIT_OK = 0
EXIT_ERROR = 1
EXIT_USAGE = 2
EXIT_VERIFY = 3


def _resolve_under(root: Optional[pathlib.Path], value: str) -> pathlib.Path:
    path = pathlib.Path(value).expanduser()
    if not path.is_absolute() and root is not None:
        path = root / path
    return path.resolve()


def parse_toml_identity_sha1(config_path: pathlib.Path) -> Optional[str]:
    """Best-effort `[identity] sha1 = "..."` reader (no external TOML dep)."""
    try:
        text = config_path.read_text(encoding="utf-8")
    except OSError:
        return None
    in_identity = False
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            in_identity = line[1:-1].strip().lower() == "identity"
            continue
        if not in_identity or "=" not in line:
            continue
        key, val = line.split("=", 1)
        if key.strip().lower() != "sha1":
            continue
        val = val.strip().strip('"').strip("'")
        return val or None
    return None


def default_out_dir_for_config(config: pathlib.Path) -> pathlib.Path:
    """``symbols/foo.toml`` → sibling ``generated/``; else ``generated/`` next to config."""
    if config.parent.name == "symbols":
        return (config.parent.parent / "generated").resolve()
    return (config.parent / "generated").resolve()


def find_gbarecomp_root(project_root: pathlib.Path) -> pathlib.Path:
    env = os.environ.get("GBARECOMP_ROOT")
    if env:
        candidate = pathlib.Path(env).expanduser().resolve()
        if (candidate / "tools" / "gba_recompile").is_dir() or (
            candidate / "gbarecomp_cli.py"
        ).is_file():
            return candidate
    # SDK pack / engine checkout: this file lives in <root>/tools/
    here = pathlib.Path(__file__).resolve().parent.parent
    if (here / "tools" / "gba_recompile").is_dir() or (here / "gbarecomp_cli.py").is_file():
        return here
    nested = project_root / "gbarecomp"
    if nested.is_dir():
        return nested.resolve()
    return here


def find_gba_recompile(project_root: pathlib.Path, gbarecomp_root: pathlib.Path) -> pathlib.Path:
    for key in ("GBARECOMP_RECOMPILE", "GBARECOMP_CORE"):
        env = os.environ.get(key)
        if env:
            path = pathlib.Path(env).expanduser().resolve()
            if path.is_file():
                return path
    names = ("gba_recompile", "gba_recompile.exe", "gba_recompile-core.exe")
    search_roots = (
        gbarecomp_root,
        project_root,
        gbarecomp_root / "build",
        project_root / "build",
        gbarecomp_root / "build" / "Release",
        project_root / "build" / "Release",
        gbarecomp_root / "build" / "cli-release" / "core" / "Release",
    )
    for root in search_roots:
        for name in names:
            candidate = root / name
            if candidate.is_file() and os.access(candidate, os.X_OK):
                return candidate.resolve()
            # Windows: executable bit may be unset
            if candidate.is_file() and name.endswith(".exe"):
                return candidate.resolve()
    for name in names:
        which = shutil.which(name)
        if which:
            return pathlib.Path(which).resolve()
    raise FileNotFoundError(
        "gba_recompile binary not found. Build it from gbarecomp "
        "(target gba_recompile) or set GBARECOMP_RECOMPILE."
    )


def run_recompile(
    tool: pathlib.Path,
    args: list[str],
    *,
    cwd: pathlib.Path,
    progress: ProgressReporter,
) -> None:
    cmd = [str(tool), *args]
    progress.log(" ".join(cmd))
    proc = subprocess.run(
        cmd,
        cwd=str(cwd),
        capture_output=True,
        text=True,
        errors="replace",
    )
    for stream in (proc.stdout, proc.stderr):
        if not stream:
            continue
        for line in stream.splitlines():
            if line.strip():
                progress.log(line)
    if proc.returncode != 0:
        raise RuntimeError(f"gba_recompile failed (exit {proc.returncode})")


def bios_backend_present(gbarecomp_root: pathlib.Path) -> bool:
    gen = gbarecomp_root / "src" / "runtime" / "generated_bios"
    return (gen / "bios_recompiled.cpp").is_file() and (
        gen / "bios_dispatch_table.cpp"
    ).is_file()


def resolve_bios_paths(
    project_root: pathlib.Path,
    gbarecomp_root: pathlib.Path,
    bios_arg: str,
    bios_config_arg: str,
) -> tuple[Optional[pathlib.Path], pathlib.Path, pathlib.Path]:
    """Return (bios_bin or None, bios_config, bios_out_dir)."""
    bios_config = _resolve_under(
        gbarecomp_root,
        bios_config_arg or "bios/gba_bios.toml",
    )
    bios_out = gbarecomp_root / "src" / "runtime" / "generated_bios"
    if bios_arg:
        bios = _resolve_under(project_root, bios_arg)
        return bios, bios_config, bios_out
    defaults = (
        gbarecomp_root / "bios" / "gba_bios.bin",
        project_root / "bios" / "gba_bios.bin",
    )
    for candidate in defaults:
        if candidate.is_file():
            return candidate.resolve(), bios_config, bios_out
    return None, bios_config, bios_out


def cart_marker_ok(out_dir: pathlib.Path, marker_name: str) -> pathlib.Path:
    marker = out_dir / marker_name
    if marker.is_file():
        return marker
    if marker_name == "dispatch_table.cpp":
        shards = sorted(out_dir.glob("recompiled_[0-9][0-9][0-9].cpp"))
        if len(shards) >= 2 and (out_dir / "dispatch_table.cpp").is_file():
            return out_dir / "dispatch_table.cpp"
    raise RuntimeError(
        f"generate produced no marker {marker_name!r} under {out_dir}"
    )


def generate(
    *,
    rom: pathlib.Path,
    config: pathlib.Path,
    project_root: pathlib.Path,
    out_dir: Optional[pathlib.Path] = None,
    bios: str = "",
    bios_config: str = "",
    force_bios: bool = False,
    expected_sha1: Optional[str] = None,
    expected_crc32: Optional[str] = None,
    expected_sha256: Optional[str] = None,
    gen_marker: str = "dispatch_table.cpp",
    skip_hash_check: bool = False,
    progress: Optional[ProgressReporter] = None,
) -> dict[str, Any]:
    reporter = progress or ProgressReporter()
    project_root = pathlib.Path(project_root).resolve()
    rom = pathlib.Path(rom).resolve()
    config = pathlib.Path(config).resolve()
    if not config.is_file():
        raise ValueError(f"config not found: {config}")

    if out_dir is None:
        out_dir = default_out_dir_for_config(config)
    else:
        out_dir = pathlib.Path(out_dir).resolve()

    gbarecomp_root = find_gbarecomp_root(project_root)
    tool = find_gba_recompile(project_root, gbarecomp_root)

    if not expected_sha1 and not skip_hash_check:
        expected_sha1 = parse_toml_identity_sha1(config)

    reporter.phase("verify", pct=0.05, message=f"Verifying ROM {rom.name}")
    if skip_hash_check:
        identity = verify_rom(rom)
        identity["verified"] = False
    else:
        identity = verify_rom(
            rom,
            expected_sha1=expected_sha1,
            expected_crc32=expected_crc32,
            expected_sha256=expected_sha256,
        )
    reporter.event(
        "rom",
        path=identity["path"],
        sha1=identity["sha1"],
        crc32=identity["crc32"],
        sha256=identity["sha256"],
        size=identity["size"],
        verified=identity.get("verified", False),
    )

    bios_path, bios_cfg, bios_out = resolve_bios_paths(
        project_root, gbarecomp_root, bios, bios_config
    )
    need_bios = force_bios or not bios_backend_present(gbarecomp_root)
    if bios or need_bios:
        if bios_path is None or not bios_path.is_file():
            if bios:
                raise FileNotFoundError(f"BIOS not found: {bios}")
            reporter.log(
                "BIOS image not found; skipping BIOS regen "
                "(cart generate continues; runtime uses stub until BIOS is supplied)."
            )
        else:
            if not bios_cfg.is_file():
                raise FileNotFoundError(f"BIOS config not found: {bios_cfg}")
            reporter.phase(
                "bios_emit",
                pct=0.2,
                message=f"Generating BIOS C into {bios_out}",
            )
            run_recompile(
                tool,
                [
                    "--bios", str(bios_path),
                    "--config", str(bios_cfg),
                    "--out", str(bios_out),
                ],
                cwd=gbarecomp_root,
                progress=reporter,
            )
            if not bios_backend_present(gbarecomp_root):
                raise RuntimeError(
                    f"BIOS generate did not produce backends under {bios_out}"
                )

    out_dir.mkdir(parents=True, exist_ok=True)
    reporter.phase(
        "emit",
        pct=0.35,
        message=f"Running gba_recompile → {out_dir}",
    )
    run_recompile(
        tool,
        [
            "--rom", str(rom),
            "--config", str(config),
            "--out", str(out_dir),
        ],
        cwd=project_root,
        progress=reporter,
    )
    marker = cart_marker_ok(out_dir, gen_marker or "dispatch_table.cpp")

    result = {
        "ok": True,
        "rom": identity,
        "config": str(config),
        "out_dir": str(out_dir),
        "marker": str(marker),
        "gbarecomp_root": str(gbarecomp_root),
        "bios_present": bios_backend_present(gbarecomp_root),
    }
    reporter.phase("done", pct=1.0, message="Generate complete")
    reporter.result(**result)
    return result


def verify_rom_command(args: argparse.Namespace, progress: ProgressReporter) -> int:
    try:
        identity = verify_rom(
            pathlib.Path(args.rom),
            expected_sha1=args.expected_sha1,
            expected_crc32=args.expected_crc32,
            expected_sha256=args.expected_sha256,
        )
    except RomVerifyError as exc:
        progress.error(str(exc), code=EXIT_VERIFY, details=exc.details)
        if progress.json_progress and exc.details:
            progress.event("verify_failed", **exc.details)
        elif not progress.json_progress and exc.details:
            print(f"details: {exc.details}", file=sys.stderr)
        return EXIT_VERIFY
    except (OSError, ValueError) as exc:
        progress.error(str(exc), code=EXIT_ERROR)
        return EXIT_ERROR

    progress.phase("done", pct=1.0, message="ROM verification succeeded")
    progress.result(ok=True, rom=identity)
    if not progress.json_progress:
        print(
            f"ok sha1={identity['sha1']} crc32={identity['crc32']} "
            f"size={identity['size']} path={identity['path']}"
        )
    return EXIT_OK


def generate_command(args: argparse.Namespace, progress: ProgressReporter) -> int:
    project_root = (
        pathlib.Path(args.project_root).expanduser().resolve()
        if args.project_root
        else pathlib.Path.cwd().resolve()
    )
    config = _resolve_under(project_root, args.config)
    rom = _resolve_under(project_root, args.rom)
    out_dir = (
        _resolve_under(project_root, args.out_dir) if args.out_dir else None
    )
    try:
        generate(
            rom=rom,
            config=config,
            project_root=project_root,
            out_dir=out_dir,
            bios=args.bios or "",
            bios_config=args.bios_config or "",
            force_bios=bool(args.force_bios),
            expected_sha1=args.expected_sha1,
            expected_crc32=args.expected_crc32,
            expected_sha256=args.expected_sha256,
            gen_marker=args.gen_marker or "dispatch_table.cpp",
            skip_hash_check=bool(args.skip_hash_check),
            progress=progress,
        )
        return EXIT_OK
    except RomVerifyError as exc:
        progress.error(str(exc), code=EXIT_VERIFY, details=exc.details)
        return EXIT_VERIFY
    except FileNotFoundError as exc:
        progress.error(str(exc), code=EXIT_USAGE)
        return EXIT_USAGE
    except ValueError as exc:
        progress.error(str(exc), code=EXIT_USAGE)
        return EXIT_USAGE
    except (OSError, RuntimeError) as exc:
        progress.error(str(exc), code=EXIT_ERROR)
        return EXIT_ERROR


def resolve_embedded_toolchain_bin(project_root: pathlib.Path) -> Optional[pathlib.Path]:
    """Resolve portable toolchain bin/ (env → zip-root toolchain/ → shared cache)."""
    if resolve_pack_toolchain_bin is not None:
        return resolve_pack_toolchain_bin(project_root)
    root = project_root / "toolchain"
    if not root.is_dir():
        return None
    direct = root / "bin"
    if (direct / "cmake").is_file() or (direct / "cmake.exe").is_file():
        return direct
    try:
        kids = [p for p in root.iterdir() if p.is_dir()]
    except OSError:
        return None
    if len(kids) == 1:
        nested = kids[0] / "bin"
        if (nested / "cmake").is_file() or (nested / "cmake.exe").is_file():
            return nested
    return None


def activate_embedded_toolchain(
    project_root: pathlib.Path, progress: Optional[ProgressReporter] = None
) -> bool:
    bin_dir = resolve_embedded_toolchain_bin(project_root)
    if not bin_dir:
        return False
    if activate_toolchain_bin is not None:
        activate_toolchain_bin(
            bin_dir, log=(progress.log if progress else None)
        )
        return True
    prefix = str(bin_dir)
    cur = os.environ.get("PATH", "")
    parts = cur.split(os.pathsep) if cur else []
    if parts and pathlib.Path(parts[0]) == bin_dir:
        return True
    os.environ["PATH"] = prefix + (os.pathsep + cur if cur else "")
    if progress:
        progress.log(f"Using toolchain: {bin_dir}")
    return True


def ensure_toolchain_for_rebuild(
    project_root: pathlib.Path,
    progress: ProgressReporter,
    *,
    from_zip: str = "",
    download: bool = True,
) -> bool:
    """Ensure cmake via cache / download / offline zip (modular toolchain flow)."""
    if ensure_toolchain_pack is None:
        return activate_embedded_toolchain(project_root, progress)
    try:
        ensure_toolchain_pack(
            project_root,
            from_zip=pathlib.Path(from_zip) if from_zip else None,
            download=download and not from_zip,
            log=progress.log,
        )
        return True
    except Exception as exc:  # noqa: BLE001
        progress.log(f"Toolchain ensure: {exc}")
        return False


def ensure_toolchain_command(
    args: argparse.Namespace, progress: ProgressReporter
) -> int:
    project_root = (
        pathlib.Path(args.project_root).expanduser().resolve()
        if args.project_root
        else pathlib.Path.cwd().resolve()
    )
    zip_arg = (getattr(args, "from_zip", None) or "").strip()
    want_download = (not zip_arg) and not bool(getattr(args, "no_download", False))
    if getattr(args, "download", False):
        want_download = not zip_arg
    if ensure_toolchain_pack is None:
        progress.error("toolchain_pack.py missing", code=EXIT_ERROR)
        return EXIT_ERROR
    try:
        bin_dir = ensure_toolchain_pack(
            project_root,
            from_zip=pathlib.Path(zip_arg) if zip_arg else None,
            download=want_download,
            log=progress.log,
        )
    except Exception as exc:  # noqa: BLE001
        progress.error(str(exc), code=EXIT_ERROR)
        return EXIT_ERROR
    progress.phase("done", pct=1.0, message=f"Toolchain ready: {bin_dir}")
    progress.result(ok=True, toolchain_bin=str(bin_dir))
    return EXIT_OK


def clamp_future_mtimes(
    root: pathlib.Path,
    *,
    skip: Optional[pathlib.Path] = None,
    now: Optional[float] = None,
) -> int:
    """Clamp mtimes ahead of *now* so Ninja does not infinite-reconfigure.

    Release zips often preserve CI clocks that are slightly ahead of a user's
    clock. Ninja then treats every source as newer than ``build.ninja`` and
    fails with ``manifest 'build.ninja' still dirty after 100 tries``.
    """
    if not root.is_dir():
        return 0
    stamp = time.time() if now is None else now
    skip_res: Optional[pathlib.Path] = None
    if skip is not None:
        try:
            skip_res = skip.resolve()
        except OSError:
            skip_res = skip
    n = 0
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        dpath = pathlib.Path(dirpath)
        try:
            d_res = dpath.resolve()
        except OSError:
            d_res = dpath
        if skip_res is not None and (
            d_res == skip_res or skip_res in d_res.parents
        ):
            dirnames[:] = []
            continue
        pruned: list[str] = []
        for x in dirnames:
            if x == ".git":
                continue
            if skip_res is not None:
                try:
                    if (dpath / x).resolve() == skip_res:
                        continue
                except OSError:
                    pass
            pruned.append(x)
        dirnames[:] = pruned
        for name in filenames:
            p = dpath / name
            try:
                mtime = p.stat().st_mtime
            except OSError:
                continue
            if mtime > stamp:
                try:
                    os.utime(p, (stamp, stamp), follow_symlinks=False)
                    n += 1
                except OSError:
                    pass
    return n


def _cmake_generator_ready(build_dir: pathlib.Path) -> bool:
    """True when a prior configure produced a usable build system."""
    if not (build_dir / "CMakeCache.txt").is_file():
        return False
    return (
        (build_dir / "build.ninja").is_file()
        or (build_dir / "Makefile").is_file()
        or (build_dir / "build.make").is_file()
    )


def prune_after_rebuild(
    project_root: pathlib.Path,
    build_dir: pathlib.Path,
    modes: set[str],
    progress: ProgressReporter,
) -> None:
    if not modes:
        return
    if "toolchain" in modes or "all" in modes:
        tc = project_root / "toolchain"
        if tc.is_dir():
            shutil.rmtree(tc, ignore_errors=True)
            progress.log(f"Pruned {tc}")
    if "build-intermediates" in modes or "all" in modes:
        if build_dir.is_dir():
            for name in (
                "CMakeFiles",
                ".ninja_deps",
                ".ninja_log",
                "CMakeCache.txt",
                "cmake_install.cmake",
                "build.ninja",
                "compile_commands.json",
            ):
                p = build_dir / name
                if p.is_dir():
                    shutil.rmtree(p, ignore_errors=True)
                elif p.is_file():
                    try:
                        p.unlink()
                    except OSError:
                        pass
            for p in build_dir.rglob("*"):
                if not p.is_file():
                    continue
                if p.suffix in {".o", ".obj", ".a", ".lib", ".pdb", ".ilk", ".exp"}:
                    try:
                        p.unlink()
                    except OSError:
                        pass
            progress.log(f"Pruned build intermediates under {build_dir}")


def rebuild_command(args: argparse.Namespace, progress: ProgressReporter) -> int:
    project_root = (
        pathlib.Path(args.project_root).expanduser().resolve()
        if args.project_root
        else pathlib.Path.cwd().resolve()
    )
    build_dir = _resolve_under(project_root, args.build_dir)
    target = args.target
    if not target:
        progress.error("--target is required", code=EXIT_USAGE)
        return EXIT_USAGE

    zip_arg = (getattr(args, "toolchain_zip", None) or "").strip()
    no_dl = bool(getattr(args, "no_toolchain_download", False))
    if zip_arg:
        if not ensure_toolchain_for_rebuild(
            project_root, progress, from_zip=zip_arg, download=False
        ):
            progress.error(
                f"Failed to install toolchain from zip: {zip_arg}", code=EXIT_ERROR
            )
            return EXIT_ERROR
    elif not activate_embedded_toolchain(project_root, progress):
        if no_dl or not ensure_toolchain_for_rebuild(
            project_root, progress, download=True
        ):
            progress.error(
                "No portable toolchain and no cmake on PATH. "
                "Run: gbarecomp_cli.py ensure-toolchain --download "
                "(or pass --toolchain-zip / set GBARECOMP_TOOLCHAIN_DIR).",
                code=EXIT_ERROR,
            )
            return EXIT_ERROR

    cmake = os.environ.get("CMAKE") or shutil.which("cmake")
    if not cmake:
        progress.error("cmake not found on PATH (set CMAKE=...)", code=EXIT_ERROR)
        return EXIT_ERROR

    # Chrome " (1)" renames and similar break some Ninja POST_BUILD shell lines.
    root_s = str(project_root)
    if any(ch in root_s for ch in ("(", ")", "\n", "'")):
        progress.log(
            f"warning: project path contains shell-special characters: {project_root}. "
            "If the link step fails with \"syntax error near unexpected token\", "
            "rename/move the extract to a path without parentheses or spaces."
        )

    cmake_extra: list[str] = []
    for extra in getattr(args, "cmake_arg", None) or []:
        if extra:
            cmake_extra.append(str(extra))
    env_extra = (os.environ.get("GBARECOMP_CMAKE_EXTRA") or "").strip()
    if env_extra:
        cmake_extra.extend(env_extra.split())
    # Full playable link after local generate (not the CI setup-host shape).
    # Harmless unused cache entries when a title does not define the option.
    cmake_extra.append("-DGBARECOMP_ALLOW_NO_GENERATED=OFF")
    cmake_extra.append("-DEMERALD_FORCE_SETUP_HOST=OFF")

    clamped = clamp_future_mtimes(project_root, skip=build_dir)
    if clamped:
        progress.log(
            f"Clamped {clamped} future mtime(s) under {project_root} "
            "(avoids Ninja dirty-manifest loop from release-zip clocks)."
        )

    build_dir.mkdir(parents=True, exist_ok=True)
    if (build_dir / "CMakeCache.txt").is_file() and not _cmake_generator_ready(
        build_dir
    ):
        progress.log(
            f"Incomplete cmake tree under {build_dir} — wiping and reconfiguring"
        )
        shutil.rmtree(build_dir, ignore_errors=True)
        build_dir.mkdir(parents=True, exist_ok=True)

    # Always reconfigure (BPE-style) so generated cart C and FORCE_SETUP_HOST=OFF
    # are picked up even when a prior setup-host cache exists.
    progress.phase(
        "configure", pct=0.05, message=f"cmake -S {project_root} -B {build_dir}"
    )
    gen: list[str] = []
    if not (build_dir / "CMakeCache.txt").is_file() and shutil.which("ninja"):
        gen = ["-G", "Ninja"]
    cfg_cmd = [
        cmake,
        "-S",
        str(project_root),
        "-B",
        str(build_dir),
        *gen,
        "-DCMAKE_BUILD_TYPE=Release",
        *cmake_extra,
    ]
    progress.log(" ".join(cfg_cmd))
    proc = subprocess.run(
        cfg_cmd,
        cwd=str(project_root),
        capture_output=True,
        text=True,
        errors="replace",
    )
    for stream in (proc.stdout, proc.stderr):
        if stream:
            for line in stream.splitlines():
                if line.strip():
                    progress.log(line)
    if proc.returncode != 0:
        progress.error(
            f"cmake configure failed (exit {proc.returncode})", code=EXIT_ERROR
        )
        return EXIT_ERROR

    progress.phase("build", pct=0.2, message=f"cmake --build {build_dir} --target {target}")
    cmd = [cmake, "--build", str(build_dir), "--parallel", "--target", target]
    progress.log(" ".join(cmd))
    proc = subprocess.run(
        cmd,
        cwd=str(project_root),
        capture_output=True,
        text=True,
        errors="replace",
    )
    for stream in (proc.stdout, proc.stderr):
        if not stream:
            continue
        for line in stream.splitlines():
            if line.strip():
                progress.log(line)
    if proc.returncode != 0:
        progress.error(
            f"cmake --build failed (exit {proc.returncode})", code=EXIT_ERROR
        )
        return EXIT_ERROR

    exe_name = args.exe_basename or target
    if os.name == "nt" and not exe_name.lower().endswith(".exe"):
        exe_name = exe_name + ".exe"
    exe = build_dir / exe_name
    if not exe.is_file():
        hits = list(build_dir.rglob(exe_name))
        exe = hits[0] if hits else exe

    prune_raw = (getattr(args, "prune_after", None) or "").strip()
    if prune_raw:
        modes = {m.strip() for m in prune_raw.split(",") if m.strip()}
        progress.phase("prune", pct=0.95, message="Pruning toolchain / build bulk…")
        prune_after_rebuild(project_root, build_dir, modes, progress)

    progress.phase("done", pct=1.0, message="Rebuild complete")
    progress.result(ok=True, exe=str(exe), build_dir=str(build_dir), target=target)
    return EXIT_OK


def add_verify_parser(sub: Any) -> None:
    p = sub.add_parser("verify-rom", help="verify a GBA ROM digest")
    p.add_argument("--rom", required=True, help="path to a .gba ROM")
    p.add_argument("--expected-sha1", default=None, help="40-hex SHA-1")
    p.add_argument("--expected-crc32", default=None, help="8-hex CRC32")
    p.add_argument("--expected-sha256", default=None, help="64-hex SHA-256")
    p.add_argument("--json-progress", action="store_true")
    p.set_defaults(handler=verify_rom_command)


def add_generate_parser(sub: Any) -> None:
    p = sub.add_parser(
        "generate",
        help="regenerate cart (+ optional BIOS) C for an existing game project",
    )
    p.add_argument("--rom", required=True, help="path to a .gba ROM")
    p.add_argument(
        "--config",
        required=True,
        help="per-binary TOML (e.g. variants/<game>/symbols/<region>.toml)",
    )
    p.add_argument("--project-root", default="", help="game project root")
    p.add_argument(
        "--out-dir",
        default="",
        help="cart generated/ output (default: next to config)",
    )
    p.add_argument(
        "--bios",
        default="",
        help="optional retail GBA BIOS dump for BIOS backend regen",
    )
    p.add_argument(
        "--bios-config",
        default="",
        help="BIOS TOML (default: gbarecomp/bios/gba_bios.toml)",
    )
    p.add_argument(
        "--force-bios",
        action="store_true",
        help="regenerate BIOS backends even if already present",
    )
    p.add_argument("--expected-sha1", default=None)
    p.add_argument("--expected-crc32", default=None)
    p.add_argument("--expected-sha256", default=None)
    p.add_argument(
        "--skip-hash-check",
        action="store_true",
        help="skip digest checks (still validates ROM shape)",
    )
    p.add_argument(
        "--gen-marker",
        default="dispatch_table.cpp",
        help="expected marker under out-dir",
    )
    p.add_argument("--json-progress", action="store_true")
    p.set_defaults(handler=generate_command)


def add_rebuild_parser(sub: Any) -> None:
    p = sub.add_parser("rebuild", help="cmake --build an existing project")
    p.add_argument("--project-root", default="", help="game project root")
    p.add_argument("--build-dir", required=True, help="cmake build directory")
    p.add_argument("--target", required=True, help="cmake target name")
    p.add_argument("--exe-basename", default="", help="optional launch binary name")
    p.add_argument(
        "--cmake-arg",
        action="append",
        default=[],
        help="extra cmake configure arg (repeatable); also GBARECOMP_CMAKE_EXTRA",
    )
    p.add_argument(
        "--toolchain-zip",
        default="",
        help="offline cmake-clang-v1-*.zip (install into shared cache)",
    )
    p.add_argument(
        "--no-toolchain-download",
        action="store_true",
        help="do not download cmake-clang-v1 when cache/env/embedded missing",
    )
    p.add_argument(
        "--prune-after",
        default="",
        help="comma list after success: toolchain, build-intermediates, all",
    )
    p.add_argument("--json-progress", action="store_true")
    p.set_defaults(handler=rebuild_command)


def add_ensure_toolchain_parser(sub: Any) -> None:
    p = sub.add_parser(
        "ensure-toolchain",
        help="resolve / download / unpack cmake-clang-v1 into the shared cache",
    )
    p.add_argument("--project-root", default="", help="game project root")
    p.add_argument(
        "--from-zip",
        default="",
        help="install from a local cmake-clang-v1-*.zip",
    )
    p.add_argument(
        "--download",
        action="store_true",
        help="force download when no cache (default if neither --from-zip nor --no-download)",
    )
    p.add_argument(
        "--no-download",
        action="store_true",
        help="only reuse env / project toolchain/ / shared cache",
    )
    p.add_argument("--json-progress", action="store_true")
    p.set_defaults(handler=ensure_toolchain_command)
