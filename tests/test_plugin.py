# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 davhodg
"""The plugin in a pycangui window, with the helper over the scripted ring.

Installed as somebody would install it, from a copy of the plugin folder whose
bin/ecat_helper is the fake helper: nothing in the plugin knows it is being
tested. The helper is started by pycangui (``app.helper``) when the pane is
first shown, so these tests wait for it as the pane does.

Needs the helpers built: python build/build.py.
"""

import shutil
import sys
import time
from pathlib import Path

import pytest
from pycangui.core import plugin_package
from pycangui.ui.main_window import MainWindow
from PySide6.QtCore import QSettings

ROOT = Path(__file__).resolve().parents[1]
EXE = ".exe" if sys.platform == "win32" else ""
FAKE = ROOT / "ethercat" / "bin" / f"ecat_helper_fake{EXE}"
PANE = "ethercat:main"


def wait_for(app, condition, seconds: float = 10.0) -> bool:
    end = time.monotonic() + seconds
    while not condition() and time.monotonic() < end:
        app.processEvents()
        time.sleep(0.01)
    app.processEvents()
    return condition()


def plugin_with_fake_helper(where: Path) -> Path:
    """A copy of the plugin folder whose helper is the one over the scripted ring."""
    if not FAKE.is_file():
        pytest.fail(f"{FAKE.name} is not built: python build/build.py")
    folder = where / "ethercat"
    shutil.copytree(
        ROOT / "ethercat", folder, ignore=shutil.ignore_patterns("__pycache__", "*.exe", "ecat_helper*")
    )
    (folder / "bin").mkdir(exist_ok=True)
    shutil.copy2(FAKE, folder / "bin" / f"ecat_helper{EXE}")
    return folder


@pytest.fixture
def window(app, tmp_path, monkeypatch):
    monkeypatch.setenv("PYCANGUI_HOME", str(tmp_path / "home"))
    QSettings().clear()
    win = MainWindow()
    win.show()
    app.processEvents()
    plugin_package.install_folder(plugin_with_fake_helper(tmp_path), win.ctx.workspace_dir / "plugins")
    win._reload_plugins()
    app.processEvents()
    yield win
    win.close()


@pytest.fixture
def view(app, window):
    window.panes.show(PANE)
    found = window.panes.view(PANE)
    assert wait_for(app, lambda: found.adapters.count() > 0), found.status.text()
    return found


def opened_and_scanned(app, view) -> None:
    view.adapters.setCurrentIndex(view.adapters.findData("\\Device\\NPF_{FAKE-0001}"))
    view._open_or_close()
    assert wait_for(app, lambda: view.opened is not None)
    view.scan()
    assert wait_for(app, lambda: view.table.rowCount() == 2)


def test_it_installs_with_a_pane_and_no_helper_until_the_pane_is_shown(app, window):
    assert window.plugins.errors() == {}
    assert "EtherCAT" in [r.label for r in window.plugins.working()]
    assert window.panes.docks[PANE].windowTitle() == "EtherCAT"
    view = window.panes.view(PANE)
    assert not view.ring.started, "no program holding a network card until somebody looks"


def test_showing_the_pane_starts_the_helper_and_lists_the_adapters(app, view):
    assert view.ring.hello["master"] == "fake ring"
    assert [view.adapters.itemData(i) for i in range(view.adapters.count())] == [
        "\\Device\\NPF_{FAKE-0001}",
        "\\Device\\NPF_{FAKE-0002}",
    ]
    assert view.open_button.isEnabled() and not view.scan_button.isEnabled()


def test_open_and_scan_fill_the_table(app, view):
    opened_and_scanned(app, view)
    rows = [
        [view.table.item(row, column).text() for column in (0, 1, 2, 3, 4)]
        for row in range(view.table.rowCount())
    ]
    assert rows == [
        ["1", "Fake drive", "0x00000ACE", "0x00000402", "PREOP"],
        ["2", "Fake IO", "0x00000ACE", "0x00001001", "PREOP"],
    ]
    assert view.chosen() == 1
    assert view.read_button.isEnabled() and view.state_buttons["OP"].isEnabled()


def test_a_refused_state_shows_where_the_slave_stayed_and_why(app, view):
    opened_and_scanned(app, view)
    view.set_state("OP")
    assert wait_for(app, lambda: view.slaves[1].get("error"))
    assert view.slaves[1]["state"] == "PREOP" and view.slaves[1]["al_status"] == 0x0011
    assert view.table.item(0, 5).text().startswith("0x0011")

    view.all_slaves.setChecked(True)
    view.set_state("SAFEOP")
    assert wait_for(app, lambda: view.slaves[2]["state"] == "SAFEOP")
    assert view.slaves[1]["state"] == "SAFEOP" and not view.slaves[1]["error"]


def test_objects_are_read_and_written(app, view):
    opened_and_scanned(app, view)
    view.index.setText("0x6060")
    view.sub.setText("0")
    view.value.setText("03")
    view.sdo_write()
    view.index.setText("0x6061")
    view.sdo_read()
    assert wait_for(app, lambda: "0x6061:0 =" in view.result.text())
    assert view.result.text().startswith("0x6061:0 = 03")


def test_an_abort_is_shown_with_its_code(app, view):
    opened_and_scanned(app, view)
    view.index.setText("0x2000")
    view.sdo_read()
    assert wait_for(app, lambda: "0x06020000" in view.result.text())


def test_closing_the_adapter_empties_the_table(app, view):
    opened_and_scanned(app, view)
    view._open_or_close()
    assert wait_for(app, lambda: view.opened is None)
    assert view.table.rowCount() == 0 and not view.scan_button.isEnabled()


def test_unloading_the_plugin_stops_the_helper(app, window, view):
    helper = view.ring._helper
    assert helper.running
    window.plugins.unload("ethercat")
    assert wait_for(app, lambda: not helper.running)
