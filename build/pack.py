# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 davhodg
"""Pack the plugin as the zip pycangui installs, and write plugins.json for a release.

    python build/pack.py                     the zip, in dist/, from ethercat/bin as it is
    python build/pack.py v0.1                the zip, refused unless the tag is VERSION
    python build/pack.py --list ZIP          plugins.json written for ZIP, a release's zip

One zip for Windows and Linux: it carries both helpers, bin/ecat_helper.exe and
bin/ecat_helper, and pycangui runs the one for the computer it is on. The CI
builds each on its own system and packs them when a tag v<VERSION> is pushed;
the scripted ring's helper, which only the tests use, is left out.

Unlike the CiA 402 plugin's, this zip is not the same from one build to the
next -- compilers stamp what they make -- so plugins.json is written from the
zip the release has, once it has it:

    gh release download v0.1 --pattern "*.zip" --dir dist
    python build/pack.py --list dist/ethercat-0.1.zip
"""

import ast
import hashlib
import json
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NAME = "ethercat"
FOLDER = ROOT / NAME
LISTING = ROOT / "plugins.json"
#: Where a release's zip is, for plugins.json.
RELEASE = "https://github.com/davhodg/pycangui-ethercat/releases/download/v{version}/{zip}"
PAGE = "https://github.com/davhodg/pycangui-ethercat"
DESCRIPTION = (
    "Scans an EtherCAT ring, sets its slaves' states, reads and writes their objects, "
    "and runs their process data."
)
#: The helpers the plugin ships, one for each system.
HELPERS = ("bin/ecat_helper.exe", "bin/ecat_helper")
#: Beside the plugin in the zip: the helpers are GPL, and SOEM's notice goes with them.
LEGAL = ("LICENSE", "NOTICE")
EPOCH = (1980, 1, 1, 0, 0, 0)


def read_info() -> dict:
    """NAME, VERSION and API_VERSION, read out of plugin.py without running it."""
    tree = ast.parse((FOLDER / "plugin.py").read_text(encoding="utf-8"))
    found = {}
    for node in tree.body:
        if isinstance(node, ast.Assign) and isinstance(node.value, ast.Constant):
            for target in node.targets:
                if isinstance(target, ast.Name):
                    found[target.id] = node.value.value
    return found


def add(archive: zipfile.ZipFile, name: str, data: bytes, mode: int = 0o644) -> None:
    member = zipfile.ZipInfo(f"{NAME}/{name}", EPOCH)
    member.compress_type = zipfile.ZIP_DEFLATED
    member.create_system = 3  # Unix, so pycangui keeps the helper's executable bit
    member.external_attr = mode << 16
    archive.writestr(member, data)


def pack(dest: Path) -> Path:
    missing = [helper for helper in HELPERS if not (FOLDER / helper).is_file()]
    if missing:
        sys.exit(f"Not built: {', '.join(missing)}. Each is built on its own system.")
    sources = sorted(
        path
        for path in FOLDER.rglob("*.py")
        if "__pycache__" not in path.parts and "bin" not in path.relative_to(FOLDER).parts
    )
    dest.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(dest, "w") as archive:
        for path in sources:
            text = path.read_bytes().replace(b"\r\n", b"\n")
            add(archive, path.relative_to(FOLDER).as_posix(), text)
        for helper in HELPERS:
            add(archive, helper, (FOLDER / helper).read_bytes(), 0o755)
        for name in LEGAL:
            add(archive, name, (ROOT / name).read_bytes().replace(b"\r\n", b"\n"))
    return dest


def entry(zip_path: Path, info: dict) -> dict:
    version = str(info["VERSION"])
    return {
        "name": NAME,
        "title": info.get("NAME", NAME),
        "description": DESCRIPTION,
        "version": version,
        "api_version": int(info.get("API_VERSION", 1)),
        "url": RELEASE.format(version=version, zip=zip_path.name),
        "sha256": hashlib.sha256(zip_path.read_bytes()).hexdigest(),
        "page": PAGE,
    }


def main(args: list[str]) -> int:
    info = read_info()
    version = str(info["VERSION"])
    if "--list" in args:
        released = Path(args[args.index("--list") + 1])
        if released.name != f"{NAME}-{version}.zip":
            print(f"{released.name} is not the zip for VERSION {version}.", file=sys.stderr)
            return 1
        with zipfile.ZipFile(released) as archive:
            names = set(archive.namelist())
        if not {f"{NAME}/{helper}" for helper in HELPERS} <= names:
            print(f"{released.name} does not carry both helpers.", file=sys.stderr)
            return 1
        listing = {"format": 1, "plugins": [entry(released, info)]}
        LISTING.write_text(json.dumps(listing, indent=2) + "\n", encoding="utf-8")
        print(LISTING)
        return 0
    tag = next((a for a in args if not a.startswith("--")), None)
    if tag is not None and tag.removeprefix("v") != version:
        print(f"Tag {tag} is not the plugin's VERSION, {version}.", file=sys.stderr)
        return 1
    print(pack(ROOT / "dist" / f"{NAME}-{version}.zip"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
