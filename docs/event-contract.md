# Event Contract and ABI

This document records the low-level event contract shared by the kernel eBPF producer and the
user-space C++ consumers. It is the **stable transport layer**: the planned signal-first semantic
event model (see [signal-first-design.md](signal-first-design.md)) is layered *on top of* this ABI,
not a replacement for it.

## 1. Shared Header
[`include/event_schema.h`](../include/event_schema.h) is consumed by both:
- kernel eBPF code
- user-space C++ code

This keeps ring-buffer payload compatibility explicit.

## 2. Event Envelope
All payloads begin with `event_header`:
- `ts_ns`, `type`, `size`
- `pid`, `tgid`, `ppid`
- `uid`, `gid`
- `comm`

## 3. Typed Payloads
- `process_event`
- `file_event`
- `syscall_event`
- `network_event`

### Implementation status
- **Process:** exec / fork / exit + clone-family (`clone`, `clone3`, `vfork`) outcomes.
- **File:** `openat` / `unlinkat` / `renameat2` with enter/exit pairing and final `ret`.
- **Syscall:** broad raw syscall capture via `raw_syscalls` (no allowlist gating in the active
  flow; disabled by default through `domains.syscall`).
- **Network:** the schema and kernel handlers are fully defined and the network module is compiled
  into the BPF object and enabled by default. Lifecycle events are emitted individually; successful
  socket I/O is aggregated into five-second userspace summaries. See [architecture.md](architecture.md)
  §5 and the socket-IO coverage-gap note in §7 below.

## 4. Network Event Kinds (schema)
The `network_event_kind` enum in [`event_schema.h`](../include/event_schema.h) defines the full
socket-first taxonomy. Control plane and data plane:

| Control plane | Data plane |
|---------------|-----------|
| `socket`, `socketpair` | `sendto`, `recvfrom` |
| `bind`, `connect` | `sendmsg`, `recvmsg` |
| `listen`, `accept`, `accept4` | `read`, `write` |
| `getsockname`, `getpeername` | `readv`, `writev` |
| `setsockopt`, `getsockopt` | `sendmmsg`, `recvmmsg` |
| `shutdown`, `close` | |

Each `network_event` also carries `direction`, `transport`, `flow_id`, `socket_id`, byte counters,
and bounded `local`/`remote` endpoint snapshots. Payloads are metadata-only — no protocol/TLS
inspection (deferred; see [network-design.md](network-design.md)).

## 5. Decoder Contract
`Decoder::Decode` ([`../src/decoder/decoder.cpp`](../src/decoder/decoder.cpp)) maps raw bytes to:
`std::variant<process_event, file_event, syscall_event, network_event>`

Checks, in order:
1. minimum header size
2. declared wire `size` is within bounds
3. per-type concrete payload size before `memcpy`
4. returns `std::nullopt` for unknown type or truncated payload

## 6. ABI Stability Rules
1. Append fields; do not reorder published fields.
2. Keep fixed-width integer types.
3. Keep `type` ids stable once published.
4. Keep the kernel payload compact; enrich in user space.

## 7. Known Fidelity Limitations

Intentional current behaviors, documented so consumers can tell "deliberately empty/zero" from a bug:

- **Permanently-zero fields (not yet implemented):** `syscall_event.is_enter` is always `0` in emitted
  records — only the paired *exit* record is emitted; the enter side is stashed in-kernel and
  discarded. `network_event.bytes_captured` / `bytes_truncated` are always `0` (no payload-prefix
  capture yet).
- **Endianness in endpoints:** `network_endpoint.port` is in **host** byte order, while
  `network_endpoint.addr` bytes remain in **network** byte order (emitted as raw lowercase hex). Do
  not assume a single byte order across both.
- **Exit events are not `/proc`-enrichable:** by the time a process `exit` is processed the task's
  `/proc` entry is gone, so rich context comes only from state already cached in the entity registry.
  Exec paths are captured in-kernel at the exec tracepoint so they stay reliable even for short-lived
  processes; other exit-time rich fields may be sparse.
- **Socket-IO coverage gap:** `read`/`write`/`readv`/`writev`/`sendmmsg`/`recvmmsg` and `close` are
  attributed to sockets only when the fd was observed via `socket`/`accept`/`accept4`/`socketpair`
  *after* the collector started. Sockets opened before startup, or received via `SCM_RIGHTS`/`dup`,
  are not recognized, so their I/O (and their `close`) is not captured. This gate is deliberate — it
  prevents every regular-file `read`/`write`/`close` from being mislabeled as a network event. See
  [network-design.md](network-design.md).

Relevant files:
- [`../include/event_schema.h`](../include/event_schema.h)
- `../src/decoder/decoder.h`
- `../src/decoder/decoder.cpp`
