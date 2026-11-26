# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 davhodg
"""Build the helper and test it, then put it where the plugin runs it from.

    python build/build.py               both helpers, the C tests, copied into ethercat/bin
    python build/build.py --fake-only   only the helper over the scripted ring (no SOEM)
    python build/build.py --no-test     skip the C tests

On Windows the compiler is MSVC, found with vswhere and set up with its own
vcvars64.bat, so this runs from any shell rather than only a developer prompt.
CMake and Ninja come from requirements-dev.txt.
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
#: One build folder per platform: a checkout shared with WSL builds for both,
#: and a CMake cache made for one compiler cannot be reused by the other.
OUT = ROOT / "build" / "out" / sys.platform
BIN = ROOT / "ethercat" / "bin"
EXE = ".exe" if sys.platform == "win32" else ""


def msvc_environment() -> dict:
    """The environment vcvars64.bat sets up, read back from a shell that ran it."""
    vswhere = (
        Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))
        / "Microsoft Visual Studio"
        / "Installer"
        / "vswhere.exe"
    )
    if not vswhere.is_file():
        sys.exit("No Visual Studio found: vswhere.exe is not installed.")
    install = subprocess.run(
        [
            str(vswhere),
            "-latest",
            "-products",
            "*",
            "-requires",
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "-property",
            "installationPath",
        ],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    if not install:
        sys.exit("No Visual Studio with the C++ tools (VC.Tools.x86.x64) found.")
    vcvars = Path(install) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    shown = subprocess.run(
        f'cmd /s /c ""{vcvars}" >nul && set"', capture_output=True, text=True, check=True, shell=True
    ).stdout
    env = dict(line.split("=", 1) for line in shown.splitlines() if "=" in line)
    return env


def tool(name: str) -> str:
    """CMake or Ninja: the one beside this Python first, as requirements-dev.txt installs them."""
    beside = Path(sys.executable).parent / f"{name}{EXE}"
    found = str(beside) if beside.is_file() else shutil.which(name)
    if not found:
        sys.exit(f"{name} not found: pip install -r requirements-dev.txt")
    return found


def main(args: list[str]) -> int:
    fake_only = "--fake-only" in args
    env = msvc_environment() if sys.platform == "win32" else dict(os.environ)
    cmake = tool("cmake")
    configure = [
        cmake,
        "-S",
        str(ROOT),
        "-B",
        str(OUT),
        "-G",
        "Ninja",
        f"-DCMAKE_MAKE_PROGRAM={tool('ninja')}",
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DECAT_WITH_SOEM={'OFF' if fake_only else 'ON'}",
    ]
    subprocess.run(configure, env=env, check=True)
    subprocess.run([cmake, "--build", str(OUT)], env=env, check=True)
    if "--no-test" not in args:
        ctest = str(Path(cmake).with_name(f"ctest{EXE}"))
        subprocess.run([ctest, "--test-dir", str(OUT), "--output-on-failure"], env=env, check=True)

    BIN.mkdir(parents=True, exist_ok=True)
    for name in ["ecat_helper_fake"] + ([] if fake_only else ["ecat_helper"]):
        shutil.copy2(OUT / f"{name}{EXE}", BIN / f"{name}{EXE}")
        print(BIN / f"{name}{EXE}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
