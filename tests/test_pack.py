# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 davhodg
"""The release zip: what it carries, and that pycangui installs it as it is."""

import importlib.util
import json
import os
import shutil
import zipfile
from pathlib import Path

import pytest
from pycangui.core import plugin_package

ROOT = Path(__file__).resolve().parents[1]


def load_pack():
    spec = importlib.util.spec_from_file_location("pack", ROOT / "build" / "pack.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.fixture
def pack(tmp_path, monkeypatch):
    """pack.py over a copy of the plugin, with stand-ins for the two helpers:
    each is built on its own system, so no one machine has both."""
    module = load_pack()
    root = tmp_path / "repo"
    shutil.copytree(ROOT / "ethercat", root / "ethercat", ignore=shutil.ignore_patterns("bin", "__pycache__"))
    for name in module.LEGAL:
        shutil.copy2(ROOT / name, root / name)
    (root / "ethercat" / "bin").mkdir()
    (root / "ethercat" / "bin" / "ecat_helper.exe").write_bytes(b"MZ windows")
    (root / "ethercat" / "bin" / "ecat_helper").write_bytes(b"\x7fELF linux")
    (root / "ethercat" / "bin" / "ecat_helper_fake.exe").write_bytes(b"MZ tests only")
    monkeypatch.setattr(module, "ROOT", root)
    monkeypatch.setattr(module, "FOLDER", root / "ethercat")
    monkeypatch.setattr(module, "LISTING", root / "plugins.json")
    return module


def test_the_zip_carries_both_helpers_and_not_the_fake(pack, tmp_path):
    zipped = pack.pack(tmp_path / "ethercat-0.1.zip")
    with zipfile.ZipFile(zipped) as archive:
        names = set(archive.namelist())
        modes = {info.filename: (info.external_attr >> 16) & 0o777 for info in archive.infolist()}
    assert {"ethercat/plugin.py", "ethercat/LICENSE", "ethercat/NOTICE"} <= names
    assert modes["ethercat/bin/ecat_helper"] == modes["ethercat/bin/ecat_helper.exe"] == 0o755
    assert not any("fake" in name or "__pycache__" in name for name in names)


def test_pycangui_installs_it_with_the_helper_runnable(pack, tmp_path):
    zipped = pack.pack(tmp_path / "ethercat-0.1.zip")
    installed = plugin_package.install(zipped, tmp_path / "plugins")
    assert installed.name == "ethercat"
    helper = tmp_path / "plugins" / "ethercat" / "bin" / "ecat_helper"
    assert helper.is_file() and (helper.parent / "ecat_helper.exe").is_file()
    if os.name == "posix":
        assert os.access(helper, os.X_OK)


def test_the_listing_is_written_from_the_release_zip(pack, tmp_path):
    version = pack.read_info()["VERSION"]
    zipped = pack.pack(tmp_path / f"ethercat-{version}.zip")
    assert pack.main(["--list", str(zipped)]) == 0
    (offer,) = json.loads(pack.LISTING.read_text(encoding="utf-8"))["plugins"]
    assert offer["version"] == version and offer["api_version"] == 3
    assert offer["url"].endswith(f"/v{version}/ethercat-{version}.zip")
    assert len(offer["sha256"]) == 64


def test_a_zip_of_another_version_is_not_listed(pack, tmp_path):
    zipped = pack.pack(tmp_path / "ethercat-9.9.zip")
    assert pack.main(["--list", str(zipped)]) == 1
    assert not pack.LISTING.exists()
