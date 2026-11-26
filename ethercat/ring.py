# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 davhodg
"""The helper, the connection to it, and the thread its requests run on.

The helper is started the first time the pane is shown -- not when pycangui
starts: a workspace with the plugin installed and nobody using it has no
program holding a network card. pycangui looks after it from there
(``app.helper``): stops it with the plugin or the window, says in the Event Log
if it stops on its own and offers to start it again, which calls ``_ready``
again with the new port.
"""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

from PySide6.QtCore import QObject, Signal

from .client import Client, HelperError, Worker

#: The helper, relative to the plugin's folder (".exe" is added on Windows).
HELPER = "bin/ecat_helper"


class Ring(QObject):
    """What the pane asks things of. Everything it says comes back on the GUI thread."""

    #: Connected and agreed: the hello's answer (``master``, ``driver``).
    connected = Signal(dict)
    #: Could not start, connect or agree: the error code ("no_helper",
    #: "version", "lost", ...).
    failed = Signal(str)

    def __init__(self, app) -> None:
        super().__init__()
        self.app = app
        self.hello: dict | None = None
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
        worker.submit(lambda: client.request(op, **fields), done)

    def stop(self) -> None:
        """Let go of the adapter and the connection, and stop the worker.

        The helper itself is pycangui's to stop, with the plugin or the window;
        closing the connection makes it exit too.
        """
        client, worker = self._client, self._worker
        self._client = self._worker = None
        self._helper = None
        self.hello = None
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
        if error is not None:
            self.failed.emit(error.code if isinstance(error, HelperError) else "lost")
            return
        self.hello = hello
        self.connected.emit(hello)


def _close(client: Client) -> None:
    try:
        client.request("close")
    except HelperError:
        pass
    client.close()
