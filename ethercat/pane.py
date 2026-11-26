# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 davhodg
"""The EtherCAT pane: choose an adapter, scan the ring, set states, read and write objects.

The first stage: no process data yet, so no slave stays in OP for long -- OP
needs outputs every cycle, which comes with the cyclic thread.
Everything here is a request to the helper and its answer; nothing is polled.
"""

from __future__ import annotations

from PySide6.QtCore import Qt
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

#: What the protocol's error codes mean, to somebody looking at the pane.
ERRORS = {
    "no_helper": "The helper program is missing from the plugin's bin folder.",
    "no_driver": "Npcap is not installed. EtherCAT frames are sent through Npcap: install it "
    "from npcap.com, then press Start again.",
    "version": "The helper speaks a different protocol version: reinstall the plugin.",
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

        status_row = QHBoxLayout()
        status_row.addWidget(self.status, 1)
        status_row.addWidget(self.restart)

        layout = QVBoxLayout(self)
        layout.addLayout(status_row)
        layout.addLayout(top)
        layout.addLayout(ring_row)
        layout.addWidget(self.table, 1)
        layout.addWidget(sdo)

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
        self.status.setText(error_text(error) if error is not None else "Closed.")
        self._enable()

    # --- the ring -------------------------------------------------------------------------
    def scan(self) -> None:
        self.status.setText("Scanning...")
        self.ring.call("scan", self._scanned)

    def _scanned(self, answer, error) -> None:
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
