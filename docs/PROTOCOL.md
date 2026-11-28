# The helper protocol, version 2

The plugin and the helper program talk over TCP on 127.0.0.1, one JSON object per line, UTF-8.

## Starting and stopping

- The first line of the helper's standard input is a token: up to 64 characters, new each time it
  starts, which pycangui's `app.helper` writes there and gives the plugin as `helper.token`. A
  helper that gets none, or an empty one, exits with code 1 before it listens.
- The helper listens on 127.0.0.1 on a port the system picks, and prints the port as the first line
  of its standard output. pycangui's `app.helper` reads it.
- Any program on the computer can connect to that port, so a connection is taken only once its
  hello says the token. One whose hello says another is answered `token` and dropped, and so is one
  that has said no hello within 5 s; the helper then waits for the next.
- It serves **one** connection that said the token, and exits when that connection closes or when
  its standard input closes. pycangui stops a helper by closing its standard input, so a helper
  never outlives the plugin or the window.
- Nothing else is printed on standard output. Messages for people go to standard error, which
  pycangui puts in the Event Log.

## Requests and answers

A request is a flat object, with no nesting: string keys, and values that are strings,
non-negative integers, `true` or `false`. Every request has an `id`, a number, and an `op`.
Fields a helper does not know are ignored, so a later plugin may send more than an earlier helper
reads.

```json
{"id":3,"op":"sdo_read","slave":1,"index":24641,"sub":0}
```

Each request gets one answer, in order, with the same `id`, and `ok`:

```json
{"id":3,"ok":true,"data":"5002"}
{"id":3,"ok":false,"error":"sdo_abort","abort":100794368}
```

A line that cannot be read at all is answered without an `id`: `bad_request`, or `too_long` for a
line longer than about 3 KB. Byte values (SDO data) are hex strings, two digits a byte, in the
order the slave sends them.

## The hello

The first request must be `hello`. Anything else before it is answered `hello_first`.

| Request | Answer |
|---|---|
| `{"op":"hello","version":2,"token":"..."}` | `version` (the helper's), `master` (`"SOEM 2.0.1"`), `driver` (whether Npcap is there, on Windows) |

The token is checked first. A hello without the helper's token is answered `"error":"token"`, with
nothing else, not even the version, and the connection is closed. With the token, a version the
helper does not speak is answered `"error":"version"` with the `version` it does speak, and the
hello is not taken.

## Operations

| `op` | Fields | Answer |
|---|---|---|
| `adapters` | | `adapters`: a list of `{name, desc}` |
| `open` | `adapter`: a name from `adapters` | |
| `close` | | |
| `scan` | | `slaves`: a list of slaves, in full (below). Leaves them in PRE-OP. |
| `states` | | `slaves`: each slave's `position`, `state`, `error` and `al_status`, read again |
| `state` | `state`: `INIT`, `PREOP`, `BOOT`, `SAFEOP` or `OP`; `slave` (0 or absent: all); `timeout_ms` (default 2000, up to 60000) | `slaves`, as for `states`, whether it worked or not. `SAFEOP` and `OP` need `map` first. |
| `sdo_read` | `slave` (1 for the first), `index`, `sub` | `data` |
| `sdo_write` | `slave`, `index`, `sub`, `data` | |
| `map` | `period_us` (default 4000, 1000 to 100000) | Maps the process data and starts the cycle (below). `period_us`, `out_len`, `in_len`, `expected_wkc`, `slaves` (states), `areas`, `entries` |
| `pdo_write` | `slave`, `data`, and either `index` and `sub` (an output in `entries`) or `bit` and `bits` (within the slave's own outputs) | |
| `process` | | `running`, `period_us`, `cycles`, `wkc`, `expected_wkc`, `wkc_errors`, `late`, `outputs`, `inputs` |
| `stop` | | Every slave to PRE-OP, then the cycle stops and the map is forgotten. `slaves`, as for `states`. Stops even when a slave refuses (`state_refused`). |

A slave in full:

```json
{"position":1,"name":"Drive","vendor":2766,"product":1026,"revision":65536,
 "mailbox":true,"state":"PREOP","error":false,"al_status":0}
```

`state` is `NONE` for a slave that has not answered. `error` is the error bit of its AL status
register, set when it refused a state change. `al_status` is its AL status code, which says why.

## Process data

`map` needs every slave in PRE-OP. It lays each slave's process data out one after another, the
outputs (master to slaves) in one image and the inputs in another, asks the slaves for SAFE-OP,
and starts the helper's cyclic thread: from then on the outputs go round the ring and the inputs
come back every `period_us`, whatever else the helper is doing. Then `state` `OP`. `stop` takes
the slaves back to PRE-OP while the cycle still runs -- so they leave OP as asked, not by their
watchdog -- then stops it; `map` can then start it again. A `scan`, `open` or `close` also stops
the cycle and forgets the map, without the state change; so does the connection closing.

Bits are counted from the first bit of the outputs or of the inputs, a byte's lowest bit first,
so a value of whole bytes at a whole byte is little-endian, as on the wire. Byte values (`data`,
`outputs`, `inputs`) are hex, as for SDOs.

- `areas`: each slave's part of the images: `slave`, `out_bit`, `out_bits`, `in_bit`, `in_bits`.
- `entries`: each object in the process data, from the slaves' PDO assignments (0x1C12 and
  0x1C13, read by SDO): `slave`, `dir` (`out` or `in`), `index`, `sub`, `bits`, `bit`. A slave
  without a mailbox has an area but no entries; write to its outputs by `bit` and `bits`.
- `pdo_write`: `data` is the value, `(bits + 7) / 8` bytes, sent from the next cycle on.
- `process`: `outputs` as being sent and `inputs` as last taken; `wkc` is the last cycle's working
  counter, `expected_wkc` what it is when every slave takes part (1 for each that sends inputs, 2
  for each that takes outputs). `wkc_errors` counts cycles that came back short after it had been
  reached: a slave dropped out. `late` counts cycles that ended after the next was due.

## Errors

| `error` | Meaning |
|---|---|
| `bad_request` | Not a request this helper takes: a field missing, of the wrong kind or out of range |
| `hello_first` | No hello yet |
| `token` | The hello did not say the helper's token: the connection is closed |
| `version` | The hello's version is not this helper's |
| `unknown_op` | No such `op` |
| `too_long` | The request line, or the answer, is too long |
| `no_driver` | Npcap is not installed (Windows) |
| `no_adapter` | The adapter could not be opened: gone, or no permission (Linux: root or CAP_NET_RAW) |
| `not_open` | No adapter is open |
| `no_slaves` | The scan found nothing |
| `no_slave` | No slave at that position |
| `no_mailbox` | The slave has no CoE mailbox |
| `sdo_abort` | The slave aborted the transfer; `abort` is the abort code |
| `timeout` | No answer in time (a slave in INIT has no mailbox running) |
| `state_refused` | A slave did not reach the state asked for; `slaves` says where each got to |
| `bad_state` | Not every slave is in PRE-OP, which `map` needs |
| `not_mapped` | SAFE-OP, OP, `pdo_write`, `process` and `stop` need `map` first |
| `no_entry` | `pdo_write` named no output in the process data |
| `failed` | Anything else the master reported |
