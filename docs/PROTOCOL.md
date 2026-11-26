# The helper protocol, version 1

The plugin and the helper program talk over TCP on 127.0.0.1, one JSON object per line, UTF-8.

## Starting and stopping

- The helper listens on 127.0.0.1 on a port the system picks, and prints the port as the first line
  of its standard output. pycangui's `app.helper` reads it.
- It takes **one** connection, and exits when that connection closes or when its standard input
  closes. pycangui stops a helper by closing its standard input, so a helper never outlives the
  plugin or the window.
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
| `{"op":"hello","version":1}` | `version` (the helper's), `master` (`"SOEM 2.0.1"`), `driver` (whether Npcap is there, on Windows) |

A version the helper does not speak is answered `"error":"version"` with the `version` it does
speak, and the hello is not taken.

## Operations

| `op` | Fields | Answer |
|---|---|---|
| `adapters` | | `adapters`: a list of `{name, desc}` |
| `open` | `adapter`: a name from `adapters` | |
| `close` | | |
| `scan` | | `slaves`: a list of slaves, in full (below). Leaves them in PRE-OP. |
| `states` | | `slaves`: each slave's `position`, `state`, `error` and `al_status`, read again |
| `state` | `state`: `INIT`, `PREOP`, `BOOT`, `SAFEOP` or `OP`; `slave` (0 or absent: all); `timeout_ms` (default 2000, up to 60000) | `slaves`, as for `states`, whether it worked or not |
| `sdo_read` | `slave` (1 for the first), `index`, `sub` | `data` |
| `sdo_write` | `slave`, `index`, `sub`, `data` | |

A slave in full:

```json
{"position":1,"name":"Drive","vendor":2766,"product":1026,"revision":65536,
 "mailbox":true,"state":"PREOP","error":false,"al_status":0}
```

`state` is `NONE` for a slave that has not answered. `error` is the error bit of its AL status
register, set when it refused a state change. `al_status` is its AL status code, which says why.

## Errors

| `error` | Meaning |
|---|---|
| `bad_request` | Not a request this helper takes: a field missing, of the wrong kind or out of range |
| `hello_first` | No hello yet |
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
| `failed` | Anything else the master reported |
