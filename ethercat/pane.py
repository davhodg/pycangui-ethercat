# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 davhodg
"""The EtherCAT pane: choose an adapter, scan the ring, set states, read and write
objects, and run the process data.

Everything here is a request to the helper and its answer. The one thing asked
again and again is the process data, while it runs and the pane is shown: a
few times a second, never a request before the last came back. The helper's
own thread sends it round the ring every cycle; the pane only looks.
"""

from __future__ import annotations

from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import (
    QAbstractItemView,
    QCheckBox,
    QComboBox,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QLineEdit,
    QPushButton,
    QSpinBox,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from .client import HelperError
from .ring import Ring

#: The states, as the helper names them, and as the buttons say them.
STATES = (("INIT", "Init"), ("PREOP", "Pre-Op"), ("SAFEOP", "Safe-Op"), ("OP", "Op"))
COLUMNS = ("Position", "Name", "Vendor", "Product", "State", "AL status")
PDO_COLUMNS = ("Slave", "Direction", "Object", "Bits", "Value")
#: How often the process data is asked for while it runs and the pane is shown.
PROCESS_MS = 100

#: What the protocol's error codes mean, to somebody looking at the pane.
ERRORS = {
    "no_helper": "The helper program is missing from the plugin's bin folder.",
    "no_driver": "Npcap is not installed. EtherCAT frames are sent through Npcap: install it "
    "from npcap.com, then press Start again.",
    "version": "The helper speaks a different protocol version: reinstall the plugin.",
    "token": "The helper refused the connection: reinstall the plugin.",
    "lost": "The helper has stopped.",
    "not_connected": "The helper is not running yet.",
    "not_open": "No adapter is open.",
    "no_adapter": "The adapter could not be opened. It may have gone, or need permission "
    "(on Linux, root or CAP_NET_RAW).",
    "no_slaves": "Nothing answered on the ring: check the cable and the slaves' power.",
    "no_slave": "There is no slave at that position.",
    "no_mailbox": "That slave has no mailbox, so it has no objects to read or write.",
    "timeout": "No answer in time. A slave in Init has no mailbox running: put it in Pre-Op.",
    "too_long": "The value is too long.",
    "state_refused": "The slave did not take that state: see its AL status.",
    "not_mapped": "Start the process data first: Safe-Op and Op need it.",
    "bad_state": "Every slave must be in Pre-Op to start the process data: scan again.",
    "no_entry": "That is not an output in the process data.",
    "bad_request": "The request was not one the helper takes.",
    "failed": "The master reported an error.",
}

#: AL status codes (ETG.1000.6): why a slave refused a state.
AL_STATUS = {
    0x0000: "No error",
    0x0001: "Unspecified error",
    0x0002: "No memory",
    0x0011: "Invalid requested state change",
    0x0012: "Unknown requested state",
    0x0013: "Bootstrap not supported",
    0x0014: "No valid firmware",
    0x0015: "Invalid mailbox configuration (BOOT)",
    0x0016: "Invalid mailbox configuration (PREOP)",
    0x0017: "Invalid sync manager configuration",
    0x0018: "No valid inputs available",
    0x0019: "No valid outputs",
    0x001A: "Synchronization error",
    0x001B: "Sync manager watchdog",
    0x001C: "Invalid sync manager types",
    0x001D: "Invalid output configuration",
    0x001E: "Invalid input configuration",
    0x001F: "Invalid watchdog configuration",
    0x0020: "Slave needs cold start",
    0x0021: "Slave needs INIT",
    0x0022: "Slave needs PREOP",
    0x0023: "Slave needs SAFEOP",
    0x0024: "Invalid input mapping",
    0x0025: "Invalid output mapping",
    0x0026: "Inconsistent settings",
    0x0027: "Freerun not supported",
    0x0028: "Synchronization not supported",
    0x0029: "Freerun needs 3 buffer mode",
    0x002A: "Background watchdog",
    0x002B: "No valid inputs and outputs",
    0x002C: "Fatal sync error",
    0x002D: "No sync error",
    0x0030: "Invalid DC sync configuration",
    0x0031: "Invalid DC latch configuration",
    0x0032: "PLL error",
    0x0033: "DC sync IO error",
    0x0034: "DC sync timeout error",
    0x0035: "DC invalid sync cycle time",
    0x0050: "EEPROM no access",
    0x0051: "EEPROM error",
    0x0060: "Slave restarted locally",
}

#: SDO abort codes (CiA 301), the ones a drive commonly sends.
SDO_ABORTS = {
    0x05030000: "Toggle bit not alternated",
    0x05040000: "SDO protocol timed out",
    0x05040001: "Command specifier not valid",
    0x05040005: "Out of memory",
    0x06010000: "Unsupported access to an object",
    0x06010001: "Attempt to read a write-only object",
    0x06010002: "Attempt to write a read-only object",
    0x06020000: "Object does not exist",
    0x06040041: "Object cannot be mapped to the PDO",
    0x06040042: "PDO length exceeded",
    0x06040043: "General parameter incompatibility",
    0x06040047: "General internal incompatibility",
    0x06060000: "Access failed due to a hardware error",
    0x06070010: "Data type does not match: length does not match",
    0x06070012: "Data type does not match: too long",
    0x06070013: "Data type does not match: too short",
    0x06090011: "Sub-index does not exist",
    0x06090030: "Value range exceeded",
    0x06090031: "Value too high",
    0x06090032: "Value too low",
    0x06090036: "Maximum value is less than minimum value",
    0x08000000: "General error",
    0x08000020: "Data cannot be transferred or stored",
    0x08000021: "Data cannot be transferred or stored: local control",
    0x08000022: "Data cannot be transferred or stored: device state",
    0x08000023: "No object dictionary",
}


def error_text(error) -> str:
    """What went wrong, said for the pane: the code's meaning, and the abort code's."""
    if not isinstance(error, HelperError):
        return f"Unexpected error: {error}"
    text = ERRORS.get(error.code, error.code)
    if error.abort is not None:
        meaning = SDO_ABORTS.get(error.abort, "unknown abort code")
        text = f"SDO abort 0x{error.abort:08X}: {meaning}"
    return text


def al_text(code: int) -> str:
    return f"0x{code:04X} {AL_STATUS.get(code, 'Unknown')}" if code else ""


def parse_number(text: str, top: int) -> int | None:
    """A number typed as hex (0x...) or decimal, from 0 to ``top``; None if it is not one."""
    try:
        value = int(text.strip(), 0)
    except ValueError:
        return None
    return value if 0 <= value <= top else None


def parse_bytes(text: str) -> bytes | None:
    """Bytes typed as hex, spaces allowed: "01 02" or "0102"; None if they are not."""
    digits = "".join(text.split())
    if not digits:
        return None
    try:
        return bytes.fromhex(digits)
    except ValueError:
        return None


def pdo_rows(mapped: dict) -> list[dict]:
    """The process data's rows: each object the map listed, then each slave's part it listed nothing for.

    A row has ``slave``, ``dir`` ("out" or "in"), ``bit`` (from the start of
    the outputs or the inputs) and ``bits``; ``index`` and ``sub`` for an
    object, or ``rel``, the bit within the slave's own part, for a slave with
    no mailbox to read its PDO assignment through.
    """
    rows = [dict(entry) for entry in mapped.get("entries", [])]
    listed = {(row["slave"], row["dir"]) for row in rows}
    for area in mapped.get("areas", []):
        for way in ("out", "in"):
            bits = area[f"{way}_bits"]
            if bits and (area["slave"], way) not in listed:
                rows.append(
                    {"slave": area["slave"], "dir": way, "bit": area[f"{way}_bit"], "bits": bits, "rel": 0}
                )
    return sorted(rows, key=lambda row: (row["slave"], row["dir"] != "out", row["bit"]))


def value_at(image: bytes, bit: int, bits: int) -> int:
    """The bits of an image, EtherCAT's way: lowest bit first, little-endian."""
    return (int.from_bytes(image, "little") >> bit) & ((1 << bits) - 1)


def parse_value(text: str, bits: int) -> int | None:
    """A value typed for ``bits`` bits, hex or decimal, negative as two's complement; None if it won't fit."""
    try:
        value = int(text.strip(), 0)
    except ValueError:
        return None
    if value < 0:
        value += 1 << bits
        if value < 1 << (bits - 1):
            return None
    return value if 0 <= value < 1 << bits else None


def show_value(data: bytes) -> str:
    """A value as the slave sent it: the bytes, and what they are as a little-endian number."""
    shown = data.hex(" ").upper()
    if 0 < len(data) <= 8:
        shown += f"  =  {int.from_bytes(data, 'little')}"
    if data and all(32 <= b < 127 for b in data):
        shown += f'  =  "{data.decode("ascii")}"'
    return shown


class EthercatView(QWidget):
    """The pane. Holds the slaves as the last answer described them, and nothing else."""

    def __init__(self, ring: Ring) -> None:
        super().__init__()
        self.ring = ring
        #: The slaves, as the last scan or state answer gave them, by position.
        self.slaves: dict[int, dict] = {}
        #: The adapter open in the helper, by name; None when none is.
        self.opened: str | None = None
        #: The map's answer while the process data runs; None when it does not.
        self.mapped: dict | None = None
        #: The process data's rows (``pdo_rows``), as the table shows them.
        self.rows: list[dict] = []
        #: The last ``process`` answer: counters, outputs and inputs.
        self.process: dict | None = None
        self._asking = False

        self.status = QLabel("Starting the helper...")
        self.status.setWordWrap(True)
        self.status.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self.restart = QPushButton("Start again")
        self.restart.setToolTip("Start the helper program again.")
        self.restart.clicked.connect(self._start_again)
        self.restart.hide()

        self.adapters = QComboBox()
        self.adapters.setToolTip("The network adapter the EtherCAT ring is plugged into.")
        self.refresh_adapters = QPushButton("Refresh")
        self.refresh_adapters.setToolTip("List the network adapters again.")
        self.refresh_adapters.clicked.connect(self.list_adapters)
        self.open_button = QPushButton("Open")
        self.open_button.setToolTip("Open the adapter for EtherCAT, or close it.")
        self.open_button.clicked.connect(self._open_or_close)
        self.scan_button = QPushButton("Scan")
        self.scan_button.setToolTip("Find the slaves on the ring. They are left in Pre-Op.")
        self.scan_button.clicked.connect(self.scan)
        self.states_button = QPushButton("Read states")
        self.states_button.setToolTip("Read every slave's state and AL status again.")
        self.states_button.clicked.connect(self.read_states)

        self.table = QTableWidget(0, len(COLUMNS))
        self.table.setHorizontalHeaderLabels(COLUMNS)
        self.table.setSelectionBehavior(QAbstractItemView.SelectRows)
        self.table.setSelectionMode(QAbstractItemView.SingleSelection)
        self.table.setEditTriggers(QAbstractItemView.NoEditTriggers)
        self.table.verticalHeader().hide()
        self.table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeToContents)
        self.table.horizontalHeader().setStretchLastSection(True)
        self.table.itemSelectionChanged.connect(self._enable)

        self.all_slaves = QCheckBox("All slaves")
        self.all_slaves.setToolTip("Ask every slave for the state, rather than the one chosen.")
        self.state_buttons: dict[str, QPushButton] = {}
        for state, label in STATES:
            button = QPushButton(label)
            button.setToolTip(f"Ask for {label}.")
            button.clicked.connect(lambda _checked=False, s=state: self.set_state(s))
            self.state_buttons[state] = button

        self.index = QLineEdit()
        self.index.setPlaceholderText("0x6041")
        self.index.setToolTip("The object's index, in hex (0x...) or decimal.")
        self.sub = QLineEdit("0")
        self.sub.setToolTip("The sub-index, in hex (0x...) or decimal.")
        self.value = QLineEdit()
        self.value.setPlaceholderText("01 00")
        self.value.setToolTip("The bytes to write, in hex, least significant first, as the slave takes them.")
        self.read_button = QPushButton("Read")
        self.read_button.setToolTip("Read the object from the chosen slave.")
        self.read_button.clicked.connect(self.sdo_read)
        self.write_button = QPushButton("Write")
        self.write_button.setToolTip("Write the bytes to the object on the chosen slave.")
        self.write_button.clicked.connect(self.sdo_write)
        self.result = QLabel()
        self.result.setWordWrap(True)
        self.result.setTextInteractionFlags(Qt.TextSelectableByMouse)

        self.period = QSpinBox()
        self.period.setRange(1, 100)
        self.period.setValue(4)
        self.period.setSuffix(" ms")
        self.period.setToolTip(
            "How often the process data goes round the ring. A few milliseconds suits\n"
            "profile position and velocity; this is not a motion controller."
        )
        self.map_button = QPushButton("Start")
        self.map_button.setToolTip(
            "Start: map every slave's process data and send it every cycle. The slaves\n"
            "must be in Pre-Op, and are in Safe-Op after: then Op.\n"
            "Stop: every slave to Pre-Op, then the cycle stops."
        )
        self.map_button.clicked.connect(self._start_or_stop)
        self.counters = QLabel()
        self.counters.setToolTip(
            "Working counter: what came back against what should, when every slave takes part.\n"
            "Errors: cycles it came back short after it had been reached -- a slave dropped out.\n"
            "Late: cycles that ended after the next was due."
        )
        self.pdo_table = QTableWidget(0, len(PDO_COLUMNS))
        self.pdo_table.setHorizontalHeaderLabels(PDO_COLUMNS)
        self.pdo_table.setSelectionBehavior(QAbstractItemView.SelectRows)
        self.pdo_table.setSelectionMode(QAbstractItemView.SingleSelection)
        self.pdo_table.setEditTriggers(QAbstractItemView.NoEditTriggers)
        self.pdo_table.verticalHeader().hide()
        self.pdo_table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeToContents)
        self.pdo_table.horizontalHeader().setStretchLastSection(True)
        self.pdo_table.itemSelectionChanged.connect(self._enable)
        self.output_value = QLineEdit()
        self.output_value.setPlaceholderText("0x0F")
        self.output_value.setToolTip(
            "The value for the chosen output, hex (0x...) or decimal; negative allowed."
        )
        self.set_button = QPushButton("Set")
        self.set_button.setToolTip("Send the value in the chosen output from the next cycle on.")
        self.set_button.clicked.connect(self.set_output)
        self.process_result = QLabel()
        self.process_result.setWordWrap(True)
        self._timer = QTimer(self)
        self._timer.setInterval(PROCESS_MS)
        self._timer.timeout.connect(self._ask_process)

        top = QHBoxLayout()
        top.addWidget(QLabel("Adapter"))
        top.addWidget(self.adapters, 1)
        top.addWidget(self.refresh_adapters)
        top.addWidget(self.open_button)

        ring_row = QHBoxLayout()
        ring_row.addWidget(self.scan_button)
        ring_row.addWidget(self.states_button)
        ring_row.addStretch(1)
        ring_row.addWidget(self.all_slaves)
        for button in self.state_buttons.values():
            ring_row.addWidget(button)

        sdo = QGroupBox("Objects (SDO)")
        grid = QGridLayout(sdo)
        grid.addWidget(QLabel("Index"), 0, 0)
        grid.addWidget(self.index, 0, 1)
        grid.addWidget(QLabel("Sub"), 0, 2)
        grid.addWidget(self.sub, 0, 3)
        grid.addWidget(self.read_button, 0, 4)
        grid.addWidget(QLabel("Value"), 1, 0)
        grid.addWidget(self.value, 1, 1, 1, 3)
        grid.addWidget(self.write_button, 1, 4)
        grid.addWidget(self.result, 2, 0, 1, 5)

        pdo = QGroupBox("Process data")
        pdo_layout = QVBoxLayout(pdo)
        pdo_top = QHBoxLayout()
        pdo_top.addWidget(QLabel("Cycle"))
        pdo_top.addWidget(self.period)
        pdo_top.addWidget(self.map_button)
        pdo_top.addWidget(self.counters, 1)
        pdo_layout.addLayout(pdo_top)
        pdo_layout.addWidget(self.pdo_table, 1)
        pdo_set = QHBoxLayout()
        pdo_set.addWidget(QLabel("Value"))
        pdo_set.addWidget(self.output_value, 1)
        pdo_set.addWidget(self.set_button)
        pdo_layout.addLayout(pdo_set)
        pdo_layout.addWidget(self.process_result)

        status_row = QHBoxLayout()
        status_row.addWidget(self.status, 1)
        status_row.addWidget(self.restart)

        layout = QVBoxLayout(self)
        layout.addLayout(status_row)
        layout.addLayout(top)
        layout.addLayout(ring_row)
        layout.addWidget(self.table, 1)
        layout.addWidget(sdo)
        layout.addWidget(pdo, 1)

        ring.connected.connect(self._connected)
        ring.failed.connect(self._failed)
        self._enable()

    # --- the helper ---------------------------------------------------------------------
    def _connected(self, hello: dict) -> None:
        self.restart.hide()
        if not hello.get("driver", True):
            self.status.setText(ERRORS["no_driver"])
            self.restart.show()
            self._enable()
            return
        self.status.setText(f"Helper running ({hello.get('master', '')}).")
        self.list_adapters()

    def _failed(self, code: str) -> None:
        self.opened = None
        self._unmapped()
        self.status.setText(ERRORS.get(code, code))
        self.restart.show()
        self._enable()

    def _start_again(self) -> None:
        self.restart.hide()
        self.status.setText("Starting the helper...")
        self.ring.stop()
        self.ring.start()

    def _connected_now(self) -> bool:
        return self.ring.hello is not None and bool(self.ring.hello.get("driver", True))

    def _enable(self) -> None:
        connected = self._connected_now()
        is_open = connected and self.opened is not None
        chosen = is_open and self.chosen() is not None
        self.adapters.setEnabled(connected and self.opened is None)
        self.refresh_adapters.setEnabled(connected and self.opened is None)
        self.open_button.setEnabled(connected and (self.opened is not None or self.adapters.count() > 0))
        self.open_button.setText("Close" if self.opened is not None else "Open")
        self.scan_button.setEnabled(is_open)
        self.states_button.setEnabled(is_open and bool(self.slaves))
        for button in self.state_buttons.values():
            button.setEnabled(is_open and bool(self.slaves))
        self.read_button.setEnabled(chosen)
        self.write_button.setEnabled(chosen)
        self.period.setEnabled(is_open and bool(self.slaves) and self.mapped is None)
        self.map_button.setEnabled(is_open and bool(self.slaves))
        self.map_button.setText("Stop" if self.mapped is not None else "Start")
        output = self.chosen_row()
        self.set_button.setEnabled(self.mapped is not None and output is not None and output["dir"] == "out")

    # --- adapters -------------------------------------------------------------------------
    def list_adapters(self) -> None:
        self.ring.call("adapters", self._listed)

    def _listed(self, answer, error) -> None:
        if error is not None:
            self.status.setText(error_text(error))
            return
        current = self.adapters.currentData()
        self.adapters.clear()
        for adapter in answer.get("adapters", []):
            self.adapters.addItem(adapter.get("desc") or adapter["name"], adapter["name"])
            self.adapters.setItemData(self.adapters.count() - 1, adapter["name"], Qt.ToolTipRole)
        if current is not None and (found := self.adapters.findData(current)) >= 0:
            self.adapters.setCurrentIndex(found)
        if not self.adapters.count():
            self.status.setText("No network adapters found.")
        self._enable()

    def _open_or_close(self) -> None:
        if self.opened is not None:
            self.ring.call("close", self._closed)
            return
        name = self.adapters.currentData()
        if name:
            self.ring.call("open", lambda answer, error: self._opened(name, error), adapter=name)

    def _opened(self, name: str, error) -> None:
        if error is not None:
            self.status.setText(error_text(error))
        else:
            self.opened = name
            self.status.setText(f"Open: {self.adapters.currentText()}")
        self._enable()

    def _closed(self, _answer, error) -> None:
        self.opened = None
        self.slaves = {}
        self._show_slaves()
        self._unmapped()
        self.status.setText(error_text(error) if error is not None else "Closed.")
        self._enable()

    # --- the ring -------------------------------------------------------------------------
    def scan(self) -> None:
        self.status.setText("Scanning...")
        self.ring.call("scan", self._scanned)

    def _scanned(self, answer, error) -> None:
        self._unmapped()  # a scan ends the process data in the helper
        if error is not None:
            self.slaves = {}
            self._show_slaves()
            self.status.setText(error_text(error))
        else:
            self.slaves = {s["position"]: s for s in answer.get("slaves", [])}
            self._show_slaves()
            self.status.setText(f"{len(self.slaves)} slave(s) found.")
            if self.table.rowCount():
                self.table.selectRow(0)
        self._enable()

    def read_states(self) -> None:
        self.ring.call("states", self._states)

    def set_state(self, state: str) -> None:
        position = 0 if self.all_slaves.isChecked() else self.chosen()
        if position is None:
            return
        self.ring.call("state", self._states, slave=position, state=state)

    def _states(self, answer, error) -> None:
        # A refused change still says where every slave got to.
        source = answer if error is None else getattr(error, "answer", {})
        for update in source.get("slaves", []):
            if (slave := self.slaves.get(update["position"])) is not None:
                slave.update(update)
        self._show_slaves()
        if error is not None:
            self.status.setText(error_text(error))
        self._enable()

    def chosen(self) -> int | None:
        """The position of the slave chosen in the table."""
        rows = self.table.selectionModel().selectedRows() if self.table.selectionModel() else []
        if not rows:
            return None
        item = self.table.item(rows[0].row(), 0)
        return int(item.text()) if item is not None else None

    def _show_slaves(self) -> None:
        chosen = self.chosen()
        self.table.setRowCount(len(self.slaves))
        for row, (position, slave) in enumerate(sorted(self.slaves.items())):
            state = slave.get("state", "NONE") + (" (error)" if slave.get("error") else "")
            cells = (
                str(position),
                slave.get("name", ""),
                f"0x{slave.get('vendor', 0):08X}",
                f"0x{slave.get('product', 0):08X}",
                state,
                al_text(slave.get("al_status", 0)),
            )
            for column, text in enumerate(cells):
                self.table.setItem(row, column, QTableWidgetItem(text))
            if position == chosen:
                self.table.selectRow(row)

    # --- objects --------------------------------------------------------------------------
    def _object(self) -> tuple[int, int, int] | None:
        position = self.chosen()
        index = parse_number(self.index.text(), 0xFFFF)
        sub = parse_number(self.sub.text(), 0xFF)
        if position is None or index is None or sub is None:
            self.result.setText(
                "Choose a slave, and give an index (0 to 0xFFFF) and a sub-index (0 to 0xFF)."
            )
            return None
        return position, index, sub

    def sdo_read(self) -> None:
        if (target := self._object()) is None:
            return
        position, index, sub = target
        self.ring.call(
            "sdo_read",
            lambda answer, error: self._read(index, sub, answer, error),
            slave=position,
            index=index,
            sub=sub,
        )

    def _read(self, index: int, sub: int, answer, error) -> None:
        where = f"0x{index:04X}:{sub}"
        if error is not None:
            self.result.setText(f"{where}: {error_text(error)}")
        else:
            self.result.setText(f"{where} = {show_value(bytes.fromhex(answer.get('data', '')))}")

    def sdo_write(self) -> None:
        if (target := self._object()) is None:
            return
        data = parse_bytes(self.value.text())
        if data is None:
            self.result.setText("Give the value as hex bytes: 01 00")
            return
        position, index, sub = target
        self.ring.call(
            "sdo_write",
            lambda answer, error: self._written(index, sub, data, error),
            slave=position,
            index=index,
            sub=sub,
            data=data.hex(),
        )

    def _written(self, index: int, sub: int, data: bytes, error) -> None:
        where = f"0x{index:04X}:{sub}"
        if error is not None:
            self.result.setText(f"{where}: {error_text(error)}")
        else:
            self.result.setText(f"{where} written: {data.hex(' ').upper()}")

    # --- process data ---------------------------------------------------------------------
    def _start_or_stop(self) -> None:
        if self.mapped is None:
            self.start_process()
        else:
            self.stop_process()

    def start_process(self) -> None:
        self.ring.call("map", self._mapped, period_us=self.period.value() * 1000)

    def stop_process(self) -> None:
        self.ring.call("stop", self._stopped)

    def _stopped(self, answer, error) -> None:
        # Stopped either way; a slave that would not go to Pre-Op is in the answer.
        self._unmapped()
        self._states(answer, error)
        if error is not None:
            self.process_result.setText(error_text(error))

    def _mapped(self, answer, error) -> None:
        if error is not None:
            self.process_result.setText(error_text(error))
            self._enable()
            return
        self.mapped = answer
        self.rows = pdo_rows(answer)
        self.process = None
        self._states(answer, None)
        self._show_rows()
        self.process_result.setText("")
        self._timer.start()
        self._ask_process()

    def _unmapped(self) -> None:
        self._timer.stop()
        self.mapped = None
        self.rows = []
        self.process = None
        self._show_rows()
        self.counters.setText("")

    def _ask_process(self) -> None:
        # Only while it runs and somebody can see it, and one request at a time:
        # an SDO on the same connection may take a while.
        if self.mapped is None or self._asking or not self.isVisible():
            return
        self._asking = True
        self.ring.call("process", self._processed)

    def _processed(self, answer, error) -> None:
        self._asking = False
        if self.mapped is None:
            return
        if error is not None:
            self.counters.setText(error_text(error))
            return
        self.process = answer
        self.counters.setText(
            f"Working counter {answer['wkc']}/{answer['expected_wkc']}, errors {answer['wkc_errors']}, "
            f"late {answer['late']}, cycles {answer['cycles']}"
        )
        self._show_values()

    def chosen_row(self) -> dict | None:
        """The process data row chosen in its table."""
        model = self.pdo_table.selectionModel()
        rows = model.selectedRows() if model else []
        return self.rows[rows[0].row()] if rows and rows[0].row() < len(self.rows) else None

    def value_of(self, row: dict) -> int | None:
        """A row's value, from the outputs being sent or the inputs last taken."""
        if self.process is None:
            return None
        image = bytes.fromhex(self.process["outputs" if row["dir"] == "out" else "inputs"])
        return value_at(image, row["bit"], row["bits"])

    def set_output(self) -> None:
        row = self.chosen_row()
        if row is None or row["dir"] != "out":
            return
        value = parse_value(self.output_value.text(), row["bits"])
        if value is None:
            self.process_result.setText(f"Give a value that fits {row['bits']} bits.")
            return
        data = value.to_bytes((row["bits"] + 7) // 8, "little").hex()
        if "index" in row:
            where = {"index": row["index"], "sub": row["sub"]}
        else:
            where = {"bit": row["rel"], "bits": row["bits"]}
        self.ring.call("pdo_write", self._output_set, slave=row["slave"], data=data, **where)

    def _output_set(self, _answer, error) -> None:
        self.process_result.setText(error_text(error) if error is not None else "")
        if error is None:
            self._ask_process()

    def _show_rows(self) -> None:
        self.pdo_table.setRowCount(len(self.rows))
        for number, row in enumerate(self.rows):
            if "index" in row:
                name = f"0x{row['index']:04X}:{row['sub']}"
            else:
                name = f"bits {row['rel']} to {row['rel'] + row['bits'] - 1}"
            cells = (
                str(row["slave"]),
                "Output" if row["dir"] == "out" else "Input",
                name,
                str(row["bits"]),
                "",
            )
            for column, text in enumerate(cells):
                self.pdo_table.setItem(number, column, QTableWidgetItem(text))
        self._enable()

    def _show_values(self) -> None:
        for number, row in enumerate(self.rows):
            value = self.value_of(row)
            digits = (row["bits"] + 3) // 4
            text = "" if value is None else f"0x{value:0{digits}X}  =  {value}"
            if (item := self.pdo_table.item(number, 4)) is not None:
                item.setText(text)
