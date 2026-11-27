# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 davhodg
"""The CiA 402 plugin driving the scripted ring's drive, both plugins installed.

The CiA 402 plugin is its own repository: here, the checkout beside this one,
or wherever PYCANGUI_CIA402 says (CI checks it out there). Without either the
tests are skipped; with PYCANGUI_CIA402 set and nothing there, they fail.
"""

import os
import sys
from pathlib import Path

import pytest
from pycangui.core import plugin_package
from pycangui.ui.main_window import MainWindow
from PySide6.QtCore import QSettings
from test_plugin import PANE, plugin_with_fake_helper, started, wait_for

from ethercat.pane import value_at

ROOT = Path(__file__).resolve().parents[1]
CIA402 = Path(os.environ.get("PYCANGUI_CIA402", ROOT.parent / "pycangui-cia402")) / "cia402"
pytestmark = pytest.mark.skipif(
    "PYCANGUI_CIA402" not in os.environ and not CIA402.is_dir(),
    reason="no pycangui-cia402 checkout beside this one",
)
DRIVE = ("ethercat", 1)
#: The scripted drive's outputs (helper/ecat_port_fake.c): bit and bits.
CONTROLWORD = (0, 16)
TARGET_VELOCITY = (56, 32)
HALT = 0x0100


@pytest.fixture
def window(app, tmp_path, monkeypatch):
    monkeypatch.setenv("PYCANGUI_HOME", str(tmp_path / "home"))
    QSettings().clear()
    win = MainWindow()
    win.show()
    app.processEvents()
    plugins = win.ctx.workspace_dir / "plugins"
    plugin_package.install_folder(plugin_with_fake_helper(tmp_path), plugins)
    plugin_package.install_folder(CIA402, plugins)
    win._reload_plugins()
    app.processEvents()
    monkeypatch.setattr(win.confirm, "ask", lambda *_args: True)  # enabling is asked first
    yield win
    win.close()


@pytest.fixture
def ring(app, window):
    """The EtherCAT pane with the process data running and both slaves in Op."""
    window.panes.show(PANE)
    view = window.panes.view(PANE)
    assert wait_for(app, lambda: view.adapters.count() > 0), view.status.text()
    started(app, view)
    view.all_slaves.setChecked(True)
    view.set_state("OP")
    assert wait_for(app, lambda: view.slaves[2]["state"] == "OP")
    return view


@pytest.fixture
def motor(app, window, ring):
    """The CiA 402 pane with the scripted drive chosen."""
    view = window.panes.view("cia402:main")
    assert wait_for(app, lambda: listed(view) >= 0)
    view.node.setCurrentIndex(listed(view))
    assert view._device() is window.devices.device(DRIVE)
    return view


def listed(view) -> int:
    """Where the drive is in the CiA 402 pane's list. Not findData: it compares
    a Python object by identity, and the key there is the provider's own tuple."""
    keys = [view.node.itemData(i) for i in range(view.node.count())]
    return keys.index(DRIVE) if DRIVE in keys else -1


def enabled(app, motor) -> None:
    motor._enable()
    assert wait_for(app, lambda: motor.statusword & 0x6F == 0x27), hex(motor.statusword)


def outputs(ring) -> bytes:
    return bytes.fromhex(ring.ring.request("process")["outputs"])


def test_the_drive_is_enabled_from_the_cia402_pane(app, window, ring, motor):
    assert motor.start_btn.isHidden(), "EtherCAT states are the EtherCAT pane's"
    enabled(app, motor)
    assert value_at(outputs(ring), *CONTROLWORD) & 0x0F == 0x0F


def turning(app, motor, ring) -> None:
    """Enabled in profile velocity, with a speed asked for and the drive moving."""
    enabled(app, motor)
    profile = profile_of(motor)
    motor._write(profile.MODE, 3, then=lambda: motor._read_back(profile.MODE_DISPLAY))
    assert wait_for(app, lambda: motor.mode == 3)
    motor.target.setValue(500)
    motor._write_target()
    assert wait_for(app, lambda: value_at(outputs(ring), *TARGET_VELOCITY) == 500)


def profile_of(motor):
    """The CiA 402 plugin's profile module (its drive.py), as pycangui loaded it."""
    return sys.modules[type(motor).__module__].cia402


def test_stopping_the_process_data_halts_the_drive_first(app, window, ring, motor):
    turning(app, motor, ring)
    sent = []
    # Connected after the CiA 402 pane's: what the outputs were once it had stopped the drive.
    window.devices.device(DRIVE).leaving.connect(lambda: sent.append(outputs(ring)))
    ring._start_or_stop()
    assert wait_for(app, lambda: ring.mapped is None)
    assert len(sent) == 1
    assert value_at(sent[0], *CONTROLWORD) & HALT
    assert value_at(sent[0], *TARGET_VELOCITY) == 0
    assert motor.statusword == 0, "halted, and what it does now is no longer known"


def test_closing_the_window_halts_the_drive_before_the_helper_stops(app, window, ring, motor):
    turning(app, motor, ring)
    sent = []
    window.devices.device(DRIVE).leaving.connect(lambda: sent.append(outputs(ring)))
    window.close()
    app.processEvents()
    assert sent and value_at(sent[0], *CONTROLWORD) & HALT
    assert value_at(sent[0], *TARGET_VELOCITY) == 0
