# System Architecture

This document explains what eTraceGen is architecturally, why it is split this way, and how the
components relate. It describes the **current implementation** and notes where behavior differs
from the eventual signal-first target (see [roadmap.md](roadmap.md)).

## 1. Problem Model
Linux systems generate high-value events across process lifecycle, file activity, syscalls, and
socket lifecycle. We want:
- low-overhead collection
- stable typing
- evolvable user-space processing
- compatibility across many Linux kernels and distributions

## 2. Core Split: Kernel vs User Space
The project is split by responsibility, not by language preference alone.

### Kernel (eBPF)
- Observe tracepoints.
- Capture minimal event context.
- Emit typed payloads into a ring buffer.
- Keep verifier-safe logic and bounded execution.

### User Space (C++)
- Receive raw ring-buffer payloads.
- Decode via the shared schema.
- Enrich, filter, and serialize.
- Own lifecycle and operator-facing diagnostics.

The guiding rule: **the kernel stays small; interpretation and policy evolve in user space.**

## 3. Domain Strategy
### Currently implemented
- Process (exec, fork, exit, clone/clone3/vfork outcomes)
- File (openat, unlinkat, renameat2 with enter/exit pairing)
- Syscall (broad raw enter/exit; off by default in config)

### Network (now built in)
- Network socket control plane + transport I/O. The handlers in
  `bpf/event_logger_network.bpf.c` are **compiled into the BPF object** (via the aggregator in §5)
  and enabled by default. Enter handlers stage their payload in a per-CPU scratch map
  (`network_scratch`) rather than on the stack, since `struct network_state_value` (~512 B) would
  otherwise exceed the verifier's stack limit.

### Later scope
- DNS, HTTP/HTTPS correlation, deeper flow/protocol context (see [network-design.md](network-design.md)).

## 4. Component Graph

```text
+---------------- Linux Kernel ----------------+
| eBPF Programs -> Ring Buffer Map (events)    |
+----------------------+-----------------------+
                       |
                       v
+---------------------- User Space -----------------------------+
| Collector -> Decoder -> Enricher -> PolicyEngine -> JsonSink  |
|                  \-> Metrics counters                         |
+---------------------------------------------------------------+
```

Runtime path inside the collector callback ([src/main.cpp](../src/main.cpp)):
`raw bytes -> decode -> self-filter -> enrich -> policy -> sink`.

> **Current vs target:** `PolicyEngine` is a permissive pass-through today
> ([src/policy/policy.cpp](../src/policy/policy.cpp)). Turning it into the stateful, noise-reducing
> control point is the core of the signal-first work — see [signal-first-design.md](signal-first-design.md)
> and [roadmap.md](roadmap.md).

## 5. Kernel Source Layout
The kernel side is split by domain, but the build entrypoint stays stable:
- Entry point / aggregator: [`../bpf/event_logger.bpf.c`](../bpf/event_logger.bpf.c)
- Shared helpers and maps: `../bpf/event_logger_common.bpf.h`
- Process domain: `../bpf/event_logger_process.bpf.c`
- File domain: `../bpf/event_logger_file.bpf.c`
- Syscall domain: `../bpf/event_logger_syscall.bpf.c`
- Network domain: `../bpf/event_logger_network.bpf.c`

> **Current state:** the aggregator `event_logger.bpf.c` includes the **process, file, syscall, and
> network** modules. Network events are produced whenever `domains.network_socket: true` (enabled
> by default), then successful socket I/O is summarized in userspace. Keeping the entrypoint stable means the network module slotted in with a single
> `#include`, while the shared maps/helpers in `event_logger_common.bpf.h` are still defined once
> (include guard).

Keeping the entrypoint stable matters because CMake, loader wiring, and docs can continue to
reference one BPF object path while the actual handlers stay small and reviewable.

## 6. File-Level Architecture Map
- Entry point: [`../src/main.cpp`](../src/main.cpp)
- Collector abstraction: `../src/collector/collector.h`
- libbpf collector: `../src/collector/collector_libbpf.cpp`
- Schema (ABI): [`../include/event_schema.h`](../include/event_schema.h)
- Decoder: `../src/decoder/decoder.*`
- Enrichment: `../src/enricher/enricher.*`
- Policy: `../src/policy/policy.*`
- Sink: `../src/sinks/json_sink.*`
- Metrics: `../src/metrics/metrics.*`
- Kernel entrypoint: [`../bpf/event_logger.bpf.c`](../bpf/event_logger.bpf.c)

For the byte-level contract between kernel and user space, see [event-contract.md](event-contract.md).
