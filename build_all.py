#!/usr/bin/env python3
"""Build and merge the Windows, Android32, and Android64 Geode packages."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Sequence


PROJECT_ROOT = Path(__file__).resolve().parent


@dataclass(frozen=True)
class Platform:
    cli_name: str
    build_dir: str
    binary_suffix: str


PLATFORMS = {
    "windows": Platform(
        cli_name="windows",
        build_dir="build" if sys.platform == "win32" else "build-windows",
        binary_suffix=".dll",
    ),
    "android32": Platform(
        cli_name="android32",
        build_dir="build-android32",
        binary_suffix=".android32.so",
    ),
    "android64": Platform(
        cli_name="android64",
        build_dir="build-android64",
        binary_suffix=".android64.so",
    ),
}


class BuildAllError(RuntimeError):
    """An expected build or packaging error."""


def command_text(command: Sequence[str]) -> str:
    if sys.platform == "win32":
        return subprocess.list2cmdline(command)
    return shlex.join(command)


def run(command: Sequence[str]) -> None:
    print(f"\n> {command_text(command)}", flush=True)
    try:
        subprocess.run(command, cwd=PROJECT_ROOT, check=True)
    except FileNotFoundError as exc:
        raise BuildAllError(f"Command not found: {command[0]}") from exc
    except subprocess.CalledProcessError as exc:
        raise BuildAllError(
            f"Command failed with exit code {exc.returncode}: {command_text(command)}"
        ) from exc


def read_project_metadata() -> dict[str, Any]:
    path = PROJECT_ROOT / "mod.json"
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise BuildAllError(f"Unable to read {path}: {exc}") from exc

    if not isinstance(data.get("id"), str) or not data["id"]:
        raise BuildAllError("mod.json must contain a non-empty string 'id'")
    if not isinstance(data.get("version"), str) or not data["version"]:
        raise BuildAllError("mod.json must contain a non-empty string 'version'")
    return data


def check_build_tools(ndk: Path | None) -> Path:
    for executable in ("geode", "cmake", "ninja"):
        if shutil.which(executable) is None:
            raise BuildAllError(f"Required executable is not on PATH: {executable}")

    raw_ndk = ndk or (
        Path(os.environ["ANDROID_NDK_ROOT"])
        if os.environ.get("ANDROID_NDK_ROOT")
        else None
    )
    if raw_ndk is None:
        raise BuildAllError(
            "Android NDK was not found. Set ANDROID_NDK_ROOT or pass --ndk PATH."
        )

    resolved_ndk = raw_ndk.expanduser().resolve()
    toolchain = resolved_ndk / "build" / "cmake" / "android.toolchain.cmake"
    if not toolchain.is_file():
        raise BuildAllError(f"Invalid Android NDK path (toolchain is missing): {resolved_ndk}")
    return resolved_ndk


def build_platform(geode: str, platform: Platform, config: str, ndk: Path) -> None:
    command = [
        geode,
        "build",
        "--platform",
        platform.cli_name,
        "--config",
        config,
    ]
    if platform.cli_name.startswith("android"):
        command.extend(["--ndk", str(ndk)])
    cmake_options = ["-DGEODE_DONT_INSTALL_MODS=ON"]
    if platform.cli_name == "windows":
        # CMake 4.4 can initialize this as an empty string for VS 2026. One of
        # Geode's dependencies expects a non-empty value while configuring.
        cmake_options.append("-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /EHsc")
    command.extend(["--", *cmake_options])
    run(command)


def package_path(mod_id: str, platform: Platform) -> Path:
    path = PROJECT_ROOT / platform.build_dir / f"{mod_id}.geode"
    if not path.is_file():
        raise BuildAllError(
            f"{platform.cli_name} build did not produce the expected package: {path}"
        )
    return path


def inspect_package(package: Path) -> tuple[dict[str, Any], list[str]]:
    try:
        with zipfile.ZipFile(package) as archive:
            names = archive.namelist()
            metadata = json.loads(archive.read("mod.json").decode("utf-8"))
    except (OSError, KeyError, UnicodeDecodeError, json.JSONDecodeError, zipfile.BadZipFile) as exc:
        raise BuildAllError(f"Invalid Geode package {package}: {exc}") from exc
    return metadata, names


def read_package_binary(package: Path, member: str) -> bytes:
    try:
        with zipfile.ZipFile(package) as archive:
            return archive.read(member)
    except (OSError, KeyError, zipfile.BadZipFile) as exc:
        raise BuildAllError(
            f"Unable to read platform binary {member} from {package}: {exc}"
        ) from exc


def validate_binary_architecture(platform_name: str, binary: bytes) -> None:
    if platform_name == "windows":
        if len(binary) < 0x40 or binary[:2] != b"MZ":
            raise BuildAllError("Windows binary is not a valid PE image")
        pe_offset = int.from_bytes(binary[0x3C:0x40], "little")
        if (
            pe_offset + 6 > len(binary)
            or binary[pe_offset:pe_offset + 4] != b"PE\0\0"
        ):
            raise BuildAllError("Windows binary has an invalid PE header")
        machine = int.from_bytes(binary[pe_offset + 4:pe_offset + 6], "little")
        if machine != 0x8664:
            raise BuildAllError(
                f"Windows binary has machine 0x{machine:04x}; expected AMD64"
            )
        return

    if len(binary) < 20 or binary[:4] != b"\x7fELF":
        raise BuildAllError(f"{platform_name} binary is not a valid ELF image")
    expected_class = 1 if platform_name == "android32" else 2
    expected_machine = 40 if platform_name == "android32" else 183
    if binary[4] != expected_class or binary[5] != 1:
        raise BuildAllError(
            f"{platform_name} binary has the wrong ELF class or byte order"
        )
    machine = int.from_bytes(binary[18:20], "little")
    if machine != expected_machine:
        raise BuildAllError(
            f"{platform_name} binary has ELF machine {machine}; "
            f"expected {expected_machine}"
        )


def validate_platform_packages(
    packages: dict[str, Path], project_metadata: dict[str, Any]
) -> None:
    expected_id = project_metadata["id"]
    expected_version = project_metadata["version"]

    for name, package in packages.items():
        metadata, members = inspect_package(package)
        if metadata.get("id") != expected_id:
            raise BuildAllError(
                f"{name} package has mod id {metadata.get('id')!r}; expected {expected_id!r}"
            )
        if metadata.get("version") != expected_version:
            raise BuildAllError(
                f"{name} package has version {metadata.get('version')!r}; "
                f"expected {expected_version!r}"
            )

        expected_binary = f"{expected_id}{PLATFORMS[name].binary_suffix}"
        if expected_binary not in members:
            raise BuildAllError(
                f"{name} package is missing platform binary: {expected_binary}"
            )
        validate_binary_architecture(
            name,
            read_package_binary(package, expected_binary),
        )


def validate_merged_package(output: Path, project_metadata: dict[str, Any]) -> None:
    metadata, members = inspect_package(output)
    if metadata.get("id") != project_metadata["id"]:
        raise BuildAllError("Merged package metadata does not match mod.json")
    if metadata.get("version") != project_metadata["version"]:
        raise BuildAllError("Merged package version does not match mod.json")

    missing = []
    for platform in PLATFORMS.values():
        expected = f"{project_metadata['id']}{platform.binary_suffix}"
        if expected not in members:
            missing.append(expected)
    if missing:
        raise BuildAllError(f"Merged package is missing binaries: {', '.join(missing)}")
    for name, platform in PLATFORMS.items():
        member = f"{project_metadata['id']}{platform.binary_suffix}"
        validate_binary_architecture(
            name,
            read_package_binary(output, member),
        )


def merge_packages(
    geode: str,
    packages: dict[str, Path],
    output: Path,
    project_metadata: dict[str, Any],
) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    resolved_inputs = {path.resolve() for path in packages.values()}
    if output.resolve() in resolved_inputs:
        raise BuildAllError("--output must not overwrite a platform-specific package")

    handle, temporary_name = tempfile.mkstemp(
        prefix=".all-platform-", suffix=".geode", dir=output.parent
    )
    os.close(handle)
    temporary = Path(temporary_name)

    try:
        # `geode package merge` appends binaries to its first argument, so merge
        # into a disposable copy and atomically publish it only after validation.
        shutil.copy2(packages["windows"], temporary)
        run(
            [
                geode,
                "package",
                "merge",
                str(temporary),
                str(packages["android32"]),
                str(packages["android64"]),
            ]
        )
        validate_merged_package(temporary, project_metadata)
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Build Windows, Android32, and Android64, then merge them into one "
            "AllPlatform.geode package."
        )
    )
    parser.add_argument(
        "--config",
        default="Release",
        help="CMake build configuration (default: Release)",
    )
    parser.add_argument(
        "--ndk",
        type=Path,
        help="Android NDK root (default: ANDROID_NDK_ROOT)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="Output .geode path (default: dist/<mod-id>-AllPlatform.geode)",
    )
    parser.add_argument(
        "--skip-build",
        action="store_true",
        help="Only validate and merge packages already present in the build folders",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        metadata = read_project_metadata()
        geode = shutil.which("geode")
        if geode is None:
            raise BuildAllError("Required executable is not on PATH: geode")

        if not args.skip_build:
            ndk = check_build_tools(args.ndk)
            for platform in PLATFORMS.values():
                build_platform(geode, platform, args.config, ndk)

        packages = {
            name: package_path(metadata["id"], platform)
            for name, platform in PLATFORMS.items()
        }
        validate_platform_packages(packages, metadata)

        if args.output:
            output = args.output
            if not output.is_absolute():
                output = PROJECT_ROOT / output
        else:
            output = PROJECT_ROOT / "dist" / f"{metadata['id']}-AllPlatform.geode"
        output = output.resolve()

        merge_packages(geode, packages, output, metadata)

        print("\nAll-platform package created successfully.")
        print(f"  Output : {output}")
        print(f"  Size   : {output.stat().st_size:,} bytes")
        print(f"  SHA-256: {sha256(output)}")
        return 0
    except BuildAllError as exc:
        print(f"\nERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
