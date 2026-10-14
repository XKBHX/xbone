#!/usr/bin/env python3

import argparse
import hashlib
import shutil
import tarfile
import tempfile
import urllib.request
from pathlib import Path


DAWN_VERSION = "v20261013.193955"
DAWN_REVISION = "f63db59722e8c1c0da9a3bcea0e40fb5c817a059"

ARTIFACTS = {
    ("linux", "x86_64"): (
        f"Dawn-{DAWN_REVISION}-ubuntu-latest-Release.tar.gz",
        "6a1b3ebecedd4aa147f8402e14dc6966883a5735a39cf68c72fbbb72fa3f53fa",
    ),
    ("macos", "x86_64"): (
        f"Dawn-{DAWN_REVISION}-macos-15-intel-Release.tar.gz",
        "215303e71522e971912435ed4368624ad1117c84f6a8e614f9777fcc01e3d190",
    ),
    ("macos", "arm64"): (
        f"Dawn-{DAWN_REVISION}-macos-latest-Release.tar.gz",
        "84b9c613db994933f53764258b93d3b5a179538ec02d551270185e712b0f42de",
    ),
    ("windows", "x86_64"): (
        f"Dawn-{DAWN_REVISION}-windows-latest-Release.tar.gz",
        "5e6a5c6f05069bec6f77cf326e47af21a138bfaf283a8ea79602466b7c2bcea7",
    ),
}


def download(url: str, destination: Path) -> None:
    request = urllib.request.Request(url, headers={"User-Agent": "xemu-build"})
    with urllib.request.urlopen(request) as response:
        with destination.open("wb") as output:
            shutil.copyfileobj(response, output)


def verify(path: Path, expected: str) -> None:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    if digest.hexdigest() != expected:
        raise RuntimeError(
            f"SHA-256 mismatch for {path.name}: "
            f"expected {expected}, got {digest.hexdigest()}"
        )


def extract(path: Path, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(path, "r:gz") as archive:
        members = archive.getmembers()
        prefix = Path(members[0].name).parts[0]
        for member in members:
            member_path = Path(member.name)
            if not member_path.parts or member_path.parts[0] != prefix:
                raise RuntimeError("Dawn archive has an unexpected layout")
            if ".." in member_path.parts:
                raise RuntimeError("Dawn archive contains an unsafe path")
            if member.issym() or member.islnk():
                link_path = Path(member.linkname)
                if link_path.is_absolute() or ".." in link_path.parts:
                    raise RuntimeError("Dawn archive contains an unsafe link")
            member.name = str(Path(*member_path.parts[1:]))
            if member.name:
                archive.extract(member, destination)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Download the pinned Dawn SDK used by xemu"
    )
    parser.add_argument(
        "--platform", choices=["linux", "macos", "windows"], required=True
    )
    parser.add_argument("--arch", choices=["x86_64", "arm64"], required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    key = (args.platform, args.arch)
    if key not in ARTIFACTS:
        parser.error(f"no pinned Dawn artifact is available for {key}")

    filename, checksum = ARTIFACTS[key]
    url = (
        f"https://github.com/google/dawn/releases/download/"
        f"{DAWN_VERSION}/{filename}"
    )
    with tempfile.TemporaryDirectory() as temporary:
        archive = Path(temporary) / filename
        print(f"Downloading {url}")
        download(url, archive)
        verify(archive, checksum)
        extract(archive, args.output)


if __name__ == "__main__":
    main()
