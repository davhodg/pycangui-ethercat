# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 davhodg
"""Each slave as a pycangui device, so a pane written for devices -- the CiA 402
pane -- drives an EtherCAT drive as it drives a CANopen node.

pycangui lists them after its CANopen nodes (``app.add_device_provider``).
Nothing here imports pycangui: a device is anything with the signals and
methods ``pycangui.core.device.Device`` has, and these have them.

An object in the process data is read and written there, everything else by
SDO. Written by SDO, an output the cycle carries would be overwritten by the
next cycle within milliseconds -- and the controlword, the mode and the
targets, which are what a drive is worked by, are what drives map.
"""

from __future__ import annotations

from collections.abc import Callable, Hashable
from typing import Any

from PySide6.QtCore import QObject, Signal

from .client import HelperError
from .pane import value_at
from .ring import Ring


class EthercatDevice(QObject):
    """One slave, by its position on the ring."""

    changed = Signal()
    state_changed = Signal()
    leaving = Signal()

    #: The ring's states are set from the EtherCAT pane, for every slave at
    #: once: one slave taken out of Op alone leaves the working counter short
    #: for all of them.
    states: tuple[str, ...] = ()

    def __init__(self, ring: Ring, key: Hashable, position: int, slave: dict) -> None:
        super().__init__()
        self.ring = ring
        self.key = key
        self.position = position
        self.identity = identity(slave)
        self.label = f"EtherCAT {position}: {slave.get('name') or 'slave'}"
        #: Told it is leaving since it was last reachable: said once, whoever says it.
        self.told = False
        self.leaving.connect(self._left)
        self._last_state = self.state()
        self._last_mapped = self._entries_now()

    def _left(self) -> None:
        self.told = True

    # --- what pycangui asks of a device ------------------------------------------------
    def available(self) -> bool:
        return self.ring.hello is not None and self.ring.opened and self.position in self.ring.slaves

    def background(self, job: Callable[[], Any], done: Callable[[Any, Any], None]) -> None:
        self.ring.background(job, done)

    def read(self, index: int, sub: int) -> bytes:
        if (entry := self._entry(index, sub)) is not None:
            process = self.ring.request("process")
            image = bytes.fromhex(process["outputs" if entry["dir"] == "out" else "inputs"])
            value = value_at(image, entry["bit"], entry["bits"])
            return value.to_bytes((entry["bits"] + 7) // 8, "little")
        answer = self.ring.request("sdo_read", slave=self.position, index=index, sub=sub)
        return bytes.fromhex(answer.get("data", ""))

    def write(self, index: int, sub: int, data: bytes) -> None:
        entry = self._entry(index, sub)
        if entry is not None and entry["dir"] == "out":
            if len(data) != (entry["bits"] + 7) // 8:
                raise HelperError("bad_request")
            self.ring.request("pdo_write", slave=self.position, index=index, sub=sub, data=bytes(data).hex())
        else:
            self.ring.request("sdo_write", slave=self.position, index=index, sub=sub, data=bytes(data).hex())

    def has(self, index: int, sub: int = 0) -> bool | None:
        # Only the process data is known; the rest of the dictionary is not read yet.
        return True if self._entry(index, sub) is not None else None

    def state(self) -> tuple[str | None, bool]:
        slave = self.ring.slaves.get(self.position)
        return (slave.get("state") if slave else None), not self.available()

    def set_state(self, name: str) -> None:
        raise NotImplementedError("EtherCAT states are set from the EtherCAT pane")

    def receives(self) -> list[tuple[int, int, str]]:
        # The master is this one: nothing else writes the outputs.
        return []

    # --- kept up to date by the provider -----------------------------------------------
    def refresh(self) -> None:
        if self.available():
            self.told = False
        if (now := self.state()) != self._last_state:
            self._last_state = now
            self.state_changed.emit()
        if (entries := self._entries_now()) != self._last_mapped:
            self._last_mapped = entries
            self.changed.emit()

    def _entries_now(self) -> list[dict]:
        mapped = self.ring.mapped or {}
        return [entry for entry in mapped.get("entries", []) if entry["slave"] == self.position]

    def _entry(self, index: int, sub: int) -> dict | None:
        mapped = self.ring.mapped or {}
        for entry in mapped.get("entries", []):
            if (entry["slave"], entry["index"], entry["sub"]) == (self.position, index, sub):
                return entry
        return None


def identity(slave: dict) -> tuple:
    """What says a slave at a position is the one that was there before."""
    return slave.get("vendor"), slave.get("product"), slave.get("name")


class EthercatProvider(QObject):
    """The slaves the last scan found, as devices. One device per slave, kept
    while the same slave is at the same position."""

    changed = Signal()

    def __init__(self, ring: Ring, prefix: str) -> None:
        super().__init__()
        self.ring = ring
        self.prefix = prefix
        self._devices: dict[int, EthercatDevice] = {}
        ring.changed.connect(self._update)
        ring.ending.connect(self._ending)

    def devices(self) -> list[EthercatDevice]:
        return [self._devices[position] for position in sorted(self._devices)]

    def device(self, key: Hashable) -> EthercatDevice | None:
        return next((device for device in self._devices.values() if device.key == key), None)

    def _ending(self) -> None:
        for device in self.devices():
            if device.available() and not device.told:
                device.leaving.emit()

    def _update(self) -> None:
        before = list(self._devices)
        kept: dict[int, EthercatDevice] = {}
        for position, slave in self.ring.slaves.items():
            device = self._devices.get(position)
            if device is None or device.identity != identity(slave):
                device = EthercatDevice(self.ring, (self.prefix, position), position, slave)
            kept[position] = device
        gone = [device for position, device in self._devices.items() if kept.get(position) is not device]
        self._devices = kept
        for device in gone:
            device.refresh()  # no longer reachable: lost, for whoever still holds it
        for device in kept.values():
            device.refresh()
        if list(self._devices) != before or gone:
            self.changed.emit()
