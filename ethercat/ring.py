# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 davhodg
"""The helper, the connection to it, and the thread its requests run on.

The helper is started the first time the pane is shown -- not when pycangui
starts: a workspace with the plugin installed and nobody using it has no
program holding a network card. pycangui looks after it from there
(``app.helper``): stops it with the plugin or the window, says in the Event Log
if it stops on its own and offers to start it again, which calls ``_ready``
again with the new port.

It also keeps what the helper's answers have said about the ring -- whether an
adapter is open, the slaves, the process data's map -- for the devices it
provides to the rest of pycangui (``devices.py``), whichever pane asked.
"""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

from PySide6.QtCore import QObject, Signal

from .client import Client, HelperError, Worker

#: The helper, relative to the plugin's folder (".exe" is added on Windows).
HELPER = "bin/ecat_helper"
#: Requests after which the slaves as they were are no longer reachable:
#: a scan finds them again, open and close let go of the adapter, stop ends
#: the process data. ``ending`` is said before any of them is sent.
ENDS = frozenset({"scan", "open", "close", "stop"})


class Ring(QObject):
    """What the pane asks things of. Everything it says comes back on the GUI thread."""

    #: Connected and agreed: the hello's answer (``master``, ``driver``).
    connected = Signal(dict)
    #: Could not start, connect or agree: the error code ("no_helper",
    #: "version", "lost", ...).
    failed = Signal(str)
    #: What is known of the ring changed: ``opened``, ``slaves`` or ``mapped``.
    changed = Signal()
    #: The slaves are about to stop being reachable as they are, and still can
    #: be written to: said before a request in ``ENDS`` is sent, and before
    #: the ring stops.
    ending = Signal()

    def __init__(self, app) -> None:
        super().__init__()
        self.app = app
        self.hello: dict | None = None
        #: Whether the helper has an adapter open.
        self.opened = False
        #: The slaves, as the last answer that said gave them, by position.
        self.slaves: dict[int, dict] = {}
        #: The map's answer while the process data runs; None when it does not.
        self.mapped: dict | None = None
        self._helper = None
        self._client: Client | None = None
        self._worker: Worker | None = None

    @property
    def started(self) -> bool:
        return self._helper is not None

    def start(self) -> None:
        """Start the helper, once; a later call does nothing while it is there."""
        if self._helper is not None:
            return
        self._worker = self._worker or Worker()
        try:
            self._helper = self.app.helper(HELPER, on_ready=self._ready)
        except (FileNotFoundError, ValueError, OSError):
            self.failed.emit("no_helper")

    def call(self, op: str, done: Callable[[Any, Any], None], **fields: Any) -> None:
        """Send a request on the worker; ``done(answer, error)`` on the GUI thread."""
        client, worker = self._client, self._worker
        if client is None or worker is None:
            done(None, HelperError("not_connected"))
            return
        if op in ENDS and self.slaves:
            self.ending.emit()

        def answered(answer, error) -> None:
            self._note(op, answer, error)
            done(answer, error)

        worker.submit(lambda: client.request(op, **fields), answered)

    def request(self, op: str, **fields: Any) -> dict:
        """Send a request and wait for its answer, on whatever thread this is:
        for a device's reads and writes, which are made inside ``background``."""
        client = self._client
        if client is None:
            raise HelperError("not_connected")
        return client.request(op, **fields)

    def background(self, job: Callable[[], Any], done: Callable[[Any, Any], None]) -> None:
        """Run ``job`` on the worker, queued with the pane's requests."""
        if self._worker is None or self._client is None:
            done(None, HelperError("not_connected"))
            return
        self._worker.submit(job, done)

    def stop(self) -> None:
        """Let go of the adapter and the connection, and stop the worker.

        The helper itself is pycangui's to stop, with the plugin or the window;
        closing the connection makes it exit too.
        """
        if self.slaves:
            self.ending.emit()
        client, worker = self._client, self._worker
        self._client = self._worker = None
        self._helper = None
        self.hello = None
        self._forget()
        if worker is not None:
            if client is not None:
                worker.submit(lambda: _close(client))
            worker.stop()

    def _ready(self, port: int | None) -> None:
        if port is None or self._worker is None:
            return
        self._worker.submit(lambda: self._connect(port), self._connected)

    def _connect(self, port: int) -> dict:
        if self._client is not None:
            self._client.close()
            self._client = None
        client = Client(port)
        try:
            hello = client.hello()
        except Exception:
            client.close()
            raise
        self._client = client
        return hello

    def _connected(self, hello: dict | None, error: Any) -> None:
        self._forget()  # a helper started again has nothing open
        if error is not None:
            self.failed.emit(error.code if isinstance(error, HelperError) else "lost")
            return
        self.hello = hello
        self.connected.emit(hello)

    def _note(self, op: str, answer: dict | None, error: Any) -> None:
        """Keep what an answer said about the ring, as the pane shows it."""
        if isinstance(error, HelperError) and error.code in ("lost", "not_connected"):
            self._forget()
            return
        if op == "open":
            self.opened = error is None
            self.slaves, self.mapped = {}, None
        elif op == "close":
            self.opened, self.slaves, self.mapped = False, {}, None
        elif op == "scan":
            self.mapped = None
            self.slaves = {} if error is not None else {s["position"]: dict(s) for s in answer["slaves"]}
        elif op in ("state", "states", "map", "stop"):
            if op == "map" and error is None:
                self.mapped = answer
            elif op == "stop":
                self.mapped = None  # stopped either way
            source = answer if error is None else getattr(error, "answer", {})
            for update in source.get("slaves", []):
                if (slave := self.slaves.get(update["position"])) is not None:
                    slave.update(update)
        else:
            return
        self.changed.emit()

    def _forget(self) -> None:
        if self.opened or self.slaves or self.mapped is not None:
            self.opened, self.slaves, self.mapped = False, {}, None
            self.changed.emit()


def _close(client: Client) -> None:
    try:
        client.request("close")
    except HelperError:
        pass
    client.close()
