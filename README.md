# pycangui-ethercat

EtherCAT for [pycangui](https://github.com/davhodg/pycangui): a plugin that scans an EtherCAT
ring, sets the slaves' states, reads and writes their objects by CoE (CANopen over EtherCAT), and
runs their process data.

A commissioning and diagnostic tool, not a motion controller. On Windows frames go out through
Npcap, which works with almost any network adapter, USB dongles included, but does not hold the
steady 1 ms cycle that cyclic synchronous modes and distributed clocks need.

**Early work.** What there is now: choose an adapter, scan, set states, read and write objects,
and run the process data -- mapped from the slaves' PDO assignments, sent every few milliseconds
by the helper's own thread, its values shown live and its outputs set by hand. Each slave a scan
finds is also a device in pycangui's device lists, its process data read and written through the
cycle and the rest of its objects by SDO, ready for the CiA 402 plugin to drive. The object
dictionary from the slave comes next.

## How it is made

```
pycangui  <-- plugin API -->  plugin (Python: the pane)
                                  |
                                  |  local socket, 127.0.0.1 only  (docs/PROTOCOL.md)
                                  v
                              helper (C, its own program)  -- SOEM --> Npcap / raw socket --> NIC
```

The helper owns the network card. pycangui starts it when the pane is first shown and stops it
with the plugin or the window, so nothing is left holding the adapter.

## Installing

It needs pycangui with plugin API 3, which is in the release after 0.6.2; until then, pycangui
from master. On Windows, install [Npcap](https://npcap.com) as well; the pane says so if it is
missing. On Linux the helper needs root or `CAP_NET_RAW` to open an adapter.

There are no releases yet.

## Developing

```
git clone --recurse-submodules https://github.com/davhodg/pycangui-ethercat
python -m venv .venv
.venv/Scripts/python -m pip install -r requirements-dev.txt
.venv/Scripts/python build/build.py
.venv/Scripts/python -m pytest -n auto
```

`build/build.py` builds two helpers and runs the C tests:

- `ecat_helper`, over SOEM, which the plugin ships;
- `ecat_helper_fake`, over a scripted ring of two slaves, which the tests use.

Both go into `ethercat/bin`. On Windows it uses MSVC, found with vswhere.

- `helper/`: the helper in C. `ecat_helper.c` is the protocol, a line in and an answer out, with
  no socket. `ecat_port.h` is everything the helper asks of the ring, with
  `ecat_port_soem.c` and `ecat_port_fake.c` behind it.
- `tests_c/`: the protocol against the scripted ring.
- `ethercat/`: the plugin, as pycangui loads it: `pane.py` the pane, `ring.py` the helper and
  what its answers said about the ring, `devices.py` each slave as a pycangui device.
- `tests/`: the helper as a running program, and the plugin in a pycangui window, both over
  the scripted ring.
  `test_with_cia402.py` drives the scripted drive from the CiA 402 plugin, which it takes
  from a [pycangui-cia402](https://github.com/davhodg/pycangui-cia402) checkout beside this
  one (or `PYCANGUI_CIA402`), and skips without one.

The C follows the project's C coding standard: fixed-width types, state in a context, no
allocation, MISRA C:2023 deviations noted where they are made.

## Licence

GPL-3.0-or-later: see [LICENSE](LICENSE) and [NOTICE](NOTICE). The helper links SOEM, which is
GPLv3. pycangui itself is Apache-2.0 and never ships this plugin.

EtherCAT® is a registered trademark and patented technology, licensed by Beckhoff Automation
GmbH, Germany. This project is not affiliated with Beckhoff or the EtherCAT Technology Group.
