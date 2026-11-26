# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 davhodg
"""Talking to the helper: one JSON line out, one JSON line back.

The protocol is in docs/PROTOCOL.md. Requests block -- a state change waits for
the slaves, an SDO for the mailbox -- so they are made on the ring's own worker
thread (``Worker``), never on the GUI thread, and one at a time, which is also
what the helper wants: it answers in order.
"""

from __future__ import annotations

import json
import queue
import socket
import threading
from collections.abc import Callable
from typing import Any

from PySide6.QtCore import QObject, Signal

#: The protocol version this plugin speaks; the helper says which it speaks.
PROTOCOL = 1
#: Long enough for the slowest request -- a state change may be given 60 s --
#: and no longer: a helper that has stopped answering closes its socket, which
#: is noticed at once, so this is only for one that has hung.
TIMEOUT_S = 65.0


class HelperError(Exception):
    """The helper answered "ok": false, or could not be reached.

    ``code`` is the protocol's error code ("sdo_abort", "no_driver", ...),
    "lost" when the connection went, and ``abort`` the SDO abort code when
    there is one. ``answer`` is the whole answer, for anything else in it --
    a refused state change comes with every slave's state.
    """

    def __init__(self, code: str, answer: dict | None = None) -> None:
        super().__init__(code)
        self.code = code
        self.answer = answer or {}
        self.abort: int | None = self.answer.get("abort")


class Client:
    """A connection to the helper. Not thread-safe: used from one worker."""

    def __init__(self, port: int, host: str = "127.0.0.1") -> None:
        self._sock = socket.create_connection((host, port), timeout=TIMEOUT_S)
        self._lines = self._sock.makefile("rb")
        self._next = 0

    def request(self, op: str, **fields: Any) -> dict:
        """Send one request and return its answer; HelperError if it was not ok."""
        self._next += 1
        ident = self._next
        line = json.dumps({"id": ident, "op": op, **fields}, separators=(",", ":")) + "\n"
        try:
            self._sock.sendall(line.encode("utf-8"))
            while True:
                raw = self._lines.readline()
                if not raw:
                    raise HelperError("lost")
                answer = json.loads(raw.decode("utf-8", errors="replace"))
                if answer.get("id") == ident or "id" not in answer:
                    break
        except OSError as err:
            raise HelperError("lost") from err
        if not answer.get("ok"):
            raise HelperError(str(answer.get("error", "failed")), answer)
        return answer

    def hello(self) -> dict:
        """Agree the protocol version: HelperError("version") if the helper's differs."""
        return self.request("hello", version=PROTOCOL)

    def close(self) -> None:
        try:
            self._lines.close()
            self._sock.close()
        except OSError:
            pass


class Worker(QObject):
    """One thread for the ring's requests, results handed back on the GUI thread.

    pycangui's own background worker is the CANopen one, which makes requests
    wait for each other's; EtherCAT has nothing to wait for there, so it has
    its own. ``done(result, error)`` is called on the thread this was made on.
    """

    _finished = Signal(object, object, object)

    def __init__(self) -> None:
        super().__init__()
        self._jobs: queue.Queue = queue.Queue()
        self._finished.connect(self._deliver)
        self._thread = threading.Thread(target=self._run, name="ethercat", daemon=True)
        self._thread.start()

    def submit(self, job: Callable[[], Any], done: Callable[[Any, Any], None] | None = None) -> None:
        self._jobs.put((job, done))

    def stop(self, wait: float = 2.0) -> None:
        self._jobs.put(None)
        self._thread.join(wait)

    def busy(self) -> bool:
        return self._jobs.unfinished_tasks > 0

    def _run(self) -> None:
        while (item := self._jobs.get()) is not None:
            job, done = item
            try:
                result, error = job(), None
            except Exception as err:  # handed to done, which decides what it means
                result, error = None, err
            try:
                self._finished.emit(done, result, error)
            except RuntimeError:  # the Qt side has gone: nobody to tell
                pass
            self._jobs.task_done()
        self._jobs.task_done()

    def _deliver(self, done, result, error) -> None:
        if done is not None:
            done(result, error)
