# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 davhodg
"""EtherCAT for pycangui: scan a ring, set states, read and write objects by CoE.

The ring is worked by a helper program (bin/ecat_helper), written in C over
SOEM, which owns the network card; this plugin is the pane, and talks to the
helper over a local socket. pycangui runs the helper for it (``app.helper``),
so a helper never outlives the window or the plugin.

A commissioning and diagnostic tool, not a motion controller: frames go out
through Npcap on Windows, which does not hold the steady 1 ms cycle that
cyclic synchronous modes need.
"""

from __future__ import annotations

from .pane import EthercatView
from .ring import Ring

API_VERSION = 3
NAME = "EtherCAT"
VERSION = "0.1"
DESCRIPTION = "Scan an EtherCAT ring, set slave states, and read and write objects by CoE."


def register(app) -> None:
    ring = Ring(app)
    pane = f"{app.plugin}:main"

    def build(_name: str) -> EthercatView:
        return EthercatView(ring)

    def shutdown(_view) -> None:
        ring.stop()

    def shown(name: str, on: bool) -> None:
        if on and name == pane:
            ring.start()

    app.add_pane("main", "EtherCAT", build, area="right", shutdown=shutdown)
    app.on_pane_shown(shown)
    app.on_closing(ring.stop)
