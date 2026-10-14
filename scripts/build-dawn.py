#!/usr/bin/env python3
"""Build the pinned Dawn release from source with Tint's SPIR-V reader.

The prebuilt macOS archives used by download-dawn.py are built without the
SPIR-V reader, which the WebGPU renderer needs while it still compiles its
shaders from GLSL through SPIR-V. This script builds the same revision with
the reader enabled and installs it in the same layout, including dawn.pc.
"""

import argparse
import importlib.util
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent


def load_download_dawn():
    spec = importlib.util.spec_from_file_location(
        "download_dawn", SCRIPT_DIR / "download-dawn.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run(*args: str, cwd: Path = None) -> None:
    print("+", " ".join(args), flush=True)
    subprocess.run(args, cwd=cwd, check=True)


def main() -> None:
    download_dawn = load_download_dawn()

    parser = argparse.ArgumentParser(
        description="Build the pinned Dawn SDK from source with the SPIR-V "
        "reader enabled"
    )
    parser.add_argument(
        "--source",
        type=Path,
        required=True,
        help="Dawn checkout directory (cloned if it does not exist)",
    )
    parser.add_argument(
        "--output", type=Path, required=True, help="install prefix"
    )
    parser.add_argument(
        "--platform",
        choices=["linux", "macos"],
        default="macos" if sys.platform == "darwin" else "linux",
    )
    args = parser.parse_args()

    source = args.source.resolve()
    if not source.exists():
        run(
            "git", "clone", "--depth", "1", "--branch",
            download_dawn.DAWN_VERSION, "https://github.com/google/dawn.git",
            str(source),
        )
    revision = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=source, check=True,
        capture_output=True, text=True,
    ).stdout.strip()
    if revision != download_dawn.DAWN_REVISION:
        raise RuntimeError(
            f"{source} is at {revision}, "
            f"expected {download_dawn.DAWN_REVISION}"
        )

    build = source / "out" / "Release"
    run(
        "cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
        "-C", str(source / ".github" / "workflows" / "dawn-ci.cmake"),
        "-DCMAKE_BUILD_TYPE=Release",
        "-DTINT_BUILD_SPV_READER=ON",
        "-DDAWN_BUILD_SAMPLES=OFF",
        "-DDAWN_BUILD_TESTS=OFF",
        "-DTINT_BUILD_TESTS=OFF",
    )
    # tint_utils_bytes is installed but only built by Tint's tests.
    run("cmake", "--build", str(build), "--target", "all", "tint_utils_bytes")
    run("cmake", "--install", str(build), "--prefix", str(args.output))
    print(download_dawn.write_pkgconfig(args.output, args.platform))


if __name__ == "__main__":
    main()
