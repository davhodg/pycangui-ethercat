# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 davhodg
"""The helper as a program: started, spoken to over its socket, and stopped.

The C tests cover each request against the scripted ring; these cover what
only the running program does -- the port on its first line, one connection,
the token it reads on stdin and will hear from nobody else, lines split across
reads, a line too long, and going when its stdin or pycangui's connection
closes -- and the client the plugin speaks to it with.

Needs the helpers built: python build/build.py.
"""

import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

import pytest

from ethercat.client import PROTOCOL, Client, HelperError

ROOT = Path(__file__).resolve().parents[1]
EXE = ".exe" if sys.platform == "win32" else ""
FAKE = ROOT / "ethercat" / "bin" / f"ecat_helper_fake{EXE}"
REAL = ROOT / "ethercat" / "bin" / f"ecat_helper{EXE}"
RING = "\\Device\\NPF_{FAKE-0001}"
EMPTY = "\\Device\\NPF_{FAKE-0002}"
#: What pycangui would write on the helper's stdin.
TOKEN = "0123456789abcdef0123456789abcdef"


def hello_line(ident: int = 1, token: str = TOKEN) -> bytes:
    hello = {"id": ident, "op": "hello", "version": PROTOCOL, "token": token}
    return json.dumps(hello).encode() + b"\n"


def start(program: Path = FAKE, **env: str) -> tuple[subprocess.Popen, int]:
    """The helper, given its token on stdin as pycangui gives it."""
    if not program.is_file():
        pytest.fail(f"{program.name} is not built: python build/build.py")
    process = subprocess.Popen(
        [str(program)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env={**os.environ, **env},
    )
    process.stdin.write(f"{TOKEN}\n".encode())
    process.stdin.flush()
    port = int(process.stdout.readline())
    return process, port


def dropped(lines) -> bool:
    """Whether the helper has closed the connection: an end of file, or on
    Windows, a write after it closed answered by the connection being reset."""
    try:
        return lines.readline() == b""
    except OSError:
        return True


@pytest.fixture
def helper():
    process, port = start()
    yield process, port
    if process.poll() is None:
        process.kill()
    process.wait(5)


@pytest.fixture
def client(helper):
    made = Client(helper[1])
    made.hello(TOKEN)
    yield made
    made.close()


def test_the_first_line_is_a_port_on_this_machine_only(helper):
    _process, port = helper
    assert 0 < port < 65536
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        sock.sendall(hello_line())
        assert json.loads(sock.makefile("rb").readline())["ok"] is True


def test_closing_stdin_stops_it(helper):
    """How pycangui stops a helper: whatever the connection is doing."""
    process, port = helper
    with socket.create_connection(("127.0.0.1", port), timeout=5):
        process.stdin.close()
        assert process.wait(5) == 0


def test_closing_the_connection_stops_it(helper):
    """So nothing is left holding the network card after the plugin has gone."""
    process, port = helper
    made = Client(port)
    made.hello(TOKEN)
    made.close()
    assert process.wait(5) == 0


def test_a_connection_with_the_wrong_token_is_told_nothing_and_dropped(helper):
    """Any program on the computer can reach the port: without the token it
    learns nothing -- not even the version -- and pycangui's still gets in."""
    process, port = helper
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        lines = sock.makefile("rb")
        sock.sendall(hello_line(token="f" * 32))
        answer = json.loads(lines.readline())
        assert (answer["ok"], answer["error"]) == (False, "token")
        assert "version" not in answer
        sock.sendall(b'{"id":2,"op":"adapters"}\n')
        assert dropped(lines)
    assert process.poll() is None, "still waiting for pycangui's connection"
    made = Client(port)
    assert made.hello(TOKEN)["version"] == PROTOCOL
    made.close()
    assert process.wait(5) == 0


def test_a_connection_that_says_nothing_does_not_keep_pycangui_out(helper):
    """One that connects first and sits there is dropped once its time for a
    hello is up, and pycangui's, waiting behind it, is answered."""
    _process, port = helper
    with socket.create_connection(("127.0.0.1", port), timeout=10) as squatter:
        made = Client(port)
        began = time.monotonic()
        assert made.hello(TOKEN)["version"] == PROTOCOL
        assert time.monotonic() - began < 8
        assert dropped(squatter.makefile("rb"))
        made.close()


def test_without_a_token_it_does_not_start():
    """Started by hand, or by a pycangui that gives none: it serves nothing."""
    if not FAKE.is_file():
        pytest.fail(f"{FAKE.name} is not built: python build/build.py")
    done = subprocess.run([str(FAKE)], input=b"", capture_output=True, timeout=10, check=False)
    assert done.returncode == 1
    assert done.stdout == b"", "no port"


def test_a_request_split_across_reads_and_two_in_one_are_both_answered(helper):
    _process, port = helper
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        lines = sock.makefile("rb")
        line = hello_line()
        sock.sendall(line[:10])
        sock.sendall(line[10:] + b'{"id":2,"op":"adapters"}\n')
        first, second = json.loads(lines.readline()), json.loads(lines.readline())
    assert (first["id"], first["ok"]) == (1, True)
    assert (second["id"], len(second["adapters"])) == (2, 2)


def test_a_line_too_long_is_answered_and_the_next_one_still_is(helper):
    _process, port = helper
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        lines = sock.makefile("rb")
        sock.sendall(hello_line())
        lines.readline()
        sock.sendall(b'{"id":2,"op":"open","adapter":"' + b"x" * 10000 + b'"}\n')
        assert json.loads(lines.readline())["error"] == "too_long"
        sock.sendall(b'{"id":3,"op":"adapters"}\n')
        assert json.loads(lines.readline())["id"] == 3


def test_the_hello_agrees_the_version(helper):
    made = Client(helper[1])
    with pytest.raises(HelperError) as refused:
        made.request("hello", version=PROTOCOL + 1, token=TOKEN)
    assert refused.value.code == "version"
    assert refused.value.answer["version"] == PROTOCOL
    hello = made.hello(TOKEN)
    assert hello["version"] == PROTOCOL and hello["driver"] is True
    made.close()


def test_scan_states_and_objects_through_the_client(client):
    names = [a["name"] for a in client.request("adapters")["adapters"]]
    assert names == [RING, EMPTY]
    client.request("open", adapter=RING)
    slaves = client.request("scan")["slaves"]
    assert [(s["position"], s["name"], s["state"]) for s in slaves] == [
        (1, "Fake drive", "PREOP"),
        (2, "Fake IO", "PREOP"),
    ]

    with pytest.raises(HelperError) as refused:
        client.request("state", slave=1, state="OP")
    assert refused.value.code == "not_mapped"
    client.request("state", slave=1, state="INIT")
    with pytest.raises(HelperError) as refused:
        client.request("state", slave=1, state="BOOT")  # BOOT only from PRE-OP
    assert refused.value.code == "state_refused"
    assert refused.value.answer["slaves"][0]["al_status"] == 0x0011
    client.request("state", slave=1, state="INIT")
    client.request("state", slave=1, state="PREOP")

    assert client.request("sdo_read", slave=1, index=0x1000, sub=0)["data"] == "92010200"
    client.request("sdo_write", slave=1, index=0x6060, sub=0, data="03")
    assert client.request("sdo_read", slave=1, index=0x6061, sub=0)["data"] == "03"
    with pytest.raises(HelperError) as aborted:
        client.request("sdo_write", slave=1, index=0x6041, sub=0, data="0000")
    assert (aborted.value.code, aborted.value.abort) == ("sdo_abort", 0x06010002)


def inputs_of(client: Client, entry: dict) -> int:
    """An input entry's value, from the inputs the cycle last took."""
    image = int.from_bytes(bytes.fromhex(client.request("process")["inputs"]), "little")
    return (image >> entry["bit"]) & ((1 << entry["bits"]) - 1)


def wait_for(check, seconds: float = 2.0) -> bool:
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if check():
            return True
        time.sleep(0.005)
    return check()


def test_a_drive_enabled_through_process_data(client):
    """Mapped, in OP, the cycle carries the controlword to the drive and its statusword back."""
    client.request("open", adapter=RING)
    client.request("scan")
    mapped = client.request("map", period_us=2000)
    assert [s["state"] for s in mapped["slaves"]] == ["SAFEOP", "SAFEOP"]
    entries = {(e["slave"], e["dir"], e["index"]): e for e in mapped["entries"]}
    statusword = entries[(1, "in", 0x6041)]
    client.request("state", state="OP")

    for control, state in ((0x06, 0x21), (0x07, 0x23), (0x0F, 0x27)):
        client.request("pdo_write", slave=1, index=0x6040, sub=0, data=control.to_bytes(2, "little").hex())
        assert wait_for(lambda state=state: inputs_of(client, statusword) & 0x6F == state)

    process = client.request("process")
    assert process["running"] is True
    assert process["wkc"] == process["expected_wkc"] == 6
    assert process["cycles"] > 0 and process["wkc_errors"] == 0

    with pytest.raises(HelperError) as refused:
        client.request("pdo_write", slave=1, index=0x6041, sub=0, data="0000")
    assert refused.value.code == "no_entry"
    client.request("scan")
    with pytest.raises(HelperError) as ended:
        client.request("process")
    assert ended.value.code == "not_mapped"


def test_an_adapter_with_nothing_on_it(client):
    client.request("open", adapter=EMPTY)
    with pytest.raises(HelperError) as nothing:
        client.request("scan")
    assert nothing.value.code == "no_slaves"


def test_without_the_driver_it_starts_and_says_so():
    """Npcap missing: the helper runs, says so in its hello, and opens nothing."""
    process, port = start(ECAT_FAKE_NO_DRIVER="1")
    try:
        made = Client(port)
        assert made.hello(TOKEN)["driver"] is False
        for op, fields in (("adapters", {}), ("open", {"adapter": RING})):
            with pytest.raises(HelperError) as refused:
                made.request(op, **fields)
            assert refused.value.code == "no_driver"
        made.close()
    finally:
        process.kill()
        process.wait(5)


def test_the_real_helper_starts_and_says_whether_it_has_its_driver():
    """Over SOEM: it starts whether or not Npcap is installed, which it says."""
    process, port = start(REAL)
    try:
        made = Client(port)
        hello = made.hello(TOKEN)
        assert hello["master"].startswith("SOEM ")
        assert isinstance(hello["driver"], bool)
        if not hello["driver"]:
            with pytest.raises(HelperError) as refused:
                made.request("adapters")
            assert refused.value.code == "no_driver"
        made.close()
    finally:
        process.kill()
        process.wait(5)
