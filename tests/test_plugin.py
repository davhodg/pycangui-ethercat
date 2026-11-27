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


def started(app, view) -> None:
    """Opened, scanned, and the process data started: the slaves in SAFE-OP."""
    opened_and_scanned(app, view)
    view.start_process()
    assert wait_for(app, lambda: view.process is not None)


def test_a_refused_state_shows_where_the_slave_stayed_and_why(app, view):
    started(app, view)
    view.set_state("INIT")
    assert wait_for(app, lambda: view.slaves[1]["state"] == "INIT")
    view.set_state("OP")
    assert wait_for(app, lambda: view.slaves[1].get("error"))
    assert view.slaves[1]["state"] == "INIT" and view.slaves[1]["al_status"] == 0x0011
    assert view.table.item(0, 5).text().startswith("0x0011")

    view.set_state("PREOP")
    view.all_slaves.setChecked(True)
    view.set_state("SAFEOP")
    assert wait_for(app, lambda: view.slaves[1]["state"] == "SAFEOP" and not view.slaves[1]["error"])
    assert view.slaves[2]["state"] == "SAFEOP"


def test_safe_op_waits_for_the_process_data(app, view):
    opened_and_scanned(app, view)
    assert view.map_button.isEnabled()
    view.set_state("SAFEOP")
    app.processEvents()
    assert wait_for(app, lambda: not view.ring._worker.busy())
    assert view.slaves[1]["state"] == "PREOP"


def choose(view, slave: int, way: str, index: int | None) -> dict:
    for number, row in enumerate(view.rows):
        if (row["slave"], row["dir"], row.get("index")) == (slave, way, index):
            view.pdo_table.selectRow(number)
            return row
    raise AssertionError(f"no row for slave {slave} {way} {index}")


def test_process_data_runs_the_drive_and_the_io(app, view):
    started(app, view)
    assert [s["state"] for s in view.slaves.values()] == ["SAFEOP", "SAFEOP"]
    assert view.map_button.isEnabled() and not view.period.isEnabled()  # Stop, not a second Start
    statusword = choose(view, 1, "in", 0x6041)
    assert not view.set_button.isEnabled(), "an input is not set"
    view.all_slaves.setChecked(True)
    view.set_state("OP")
    assert wait_for(app, lambda: view.slaves[2]["state"] == "OP")

    for control, state in ((0x06, 0x21), (0x07, 0x23), (0x0F, 0x27)):
        choose(view, 1, "out", 0x6040)
        view.output_value.setText(hex(control))
        view.set_output()
        assert wait_for(app, lambda state=state: (view.value_of(statusword) or 0) & 0x6F == state)

    io_in = choose(view, 2, "in", None)
    choose(view, 2, "out", None)
    view.output_value.setText("-91")  # 0xA5 as a signed byte
    view.set_output()
    assert wait_for(app, lambda: view.value_of(io_in) == 0xA5)
    assert view.process["wkc"] == view.process["expected_wkc"] == 6

    view.scan()
    assert wait_for(app, lambda: view.mapped is None and view.slaves[1]["state"] == "PREOP")
    assert view.map_button.isEnabled() and view.pdo_table.rowCount() == 0


def test_stop_takes_the_slaves_to_pre_op_and_start_runs_again(app, view):
    started(app, view)
    view.all_slaves.setChecked(True)
    view.set_state("OP")
    assert wait_for(app, lambda: view.slaves[2]["state"] == "OP")

    view._start_or_stop()
    assert wait_for(app, lambda: view.mapped is None)
    assert [s["state"] for s in view.slaves.values()] == ["PREOP", "PREOP"]
    assert view.pdo_table.rowCount() == 0 and view.period.isEnabled()

    view._start_or_stop()
    assert wait_for(app, lambda: view.process is not None)
    assert [s["state"] for s in view.slaves.values()] == ["SAFEOP", "SAFEOP"]


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


# --- each slave as a pycangui device -----------------------------------------------------
def on_device(app, device, job):
    """Run ``job`` where the device's reads and writes go, and wait for it."""
    told = []
    device.background(job, lambda result, error: told.append((result, error)))
    assert wait_for(app, lambda: told)
    return told[0]


def drive(window):
    return window.devices.device(("ethercat", 1))


def test_a_scan_puts_each_slave_in_pycanguis_device_list(app, window, view):
    assert [d for d in window.devices.devices() if isinstance(d.key, tuple)] == []
    opened_and_scanned(app, view)
    devices = [d for d in window.devices.devices() if isinstance(d.key, tuple)]
    assert [d.key for d in devices] == [("ethercat", 1), ("ethercat", 2)]
    assert all(d.available() for d in devices)
    assert devices[0].state() == ("PREOP", False)
    assert devices[0].states == () and devices[0].receives() == []


def test_a_drive_is_enabled_through_its_device_by_the_process_data(app, window, view):
    started(app, view)
    view.all_slaves.setChecked(True)
    view.set_state("OP")
    assert wait_for(app, lambda: view.slaves[2]["state"] == "OP")
    device = drive(window)
    assert device.has(0x6040) and device.has(0x2100) is None, "only the process data is known"

    def enable():
        seen = []
        for control in (0x06, 0x07, 0x0F):
            device.write(0x6040, 0, control.to_bytes(2, "little"))
            for _ in range(200):
                status = int.from_bytes(device.read(0x6041, 0), "little")
                if status & 0x6F == {0x06: 0x21, 0x07: 0x23, 0x0F: 0x27}[control]:
                    break
            seen.append(status & 0x6F)
        return seen

    assert on_device(app, device, enable) == ([0x21, 0x23, 0x27], None)
    assert wait_for(app, lambda: view.process and view.value_of(choose(view, 1, "out", 0x6040)) == 0x0F)


def test_an_object_outside_the_process_data_goes_by_sdo(app, window, view):
    opened_and_scanned(app, view)
    device = drive(window)

    def round_trip():
        device.write(0x2100, 0, b"\x34\x12")
        return device.read(0x2100, 0)

    assert on_device(app, device, round_trip) == (b"\x34\x12", None)


def test_a_write_from_the_gui_thread_waits_for_the_workers_request(app, window, view):
    """As the CiA 402 pane's stop does when the window closes, while the pane polls."""
    started(app, view)
    device = drive(window)
    for value in range(1, 30):
        device.write(0x6060, 0, bytes([value]))
        app.processEvents()
    assert int.from_bytes(device.read(0x6060, 0), "little") == 29


def test_a_scan_says_leaving_first_and_the_same_slave_keeps_its_device(app, window, view):
    started(app, view)
    device = drive(window)
    said = []
    device.leaving.connect(lambda: said.append(device.available() and view.mapped is not None))
    view.scan()
    assert wait_for(app, lambda: view.mapped is None and view.table.rowCount() == 2)
    assert said == [True], "while the process data still ran"
    assert drive(window) is device and device.available()


def test_stop_and_close_say_leaving_once_and_close_takes_the_devices_away(app, window, view):
    started(app, view)
    device = drive(window)
    said = []
    device.leaving.connect(lambda: said.append(device.available()))
    view._start_or_stop()
    assert wait_for(app, lambda: view.mapped is None)
    assert said == [True]
    view._open_or_close()
    assert wait_for(app, lambda: view.opened is None)
    assert said == [True, True]
    assert drive(window) is None and device.state() == (None, True)


def test_unloading_the_plugin_says_leaving_once(app, window, view):
    opened_and_scanned(app, view)
    device = drive(window)
    said = []
    device.leaving.connect(lambda: said.append(device.available()))
    window.plugins.unload("ethercat")
    app.processEvents()
    assert said == [True], "by pycangui as the provider went, and not again as the ring stopped"
    assert drive(window) is None
