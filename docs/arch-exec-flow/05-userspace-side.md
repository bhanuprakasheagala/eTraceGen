# User-Space Pipeline (Modern C++)

This document explains the current C++ pipeline and why each component exists. It remains a useful reference for the present implementation, but the longer-term design now calls for a semantic filtering layer above this pipeline.

## 1. Collector Interface
`Collector` exposes:
- `Start(cb)`
- `PollOnce(timeout_ms)`
- `ReadKernelBpfStats(out)`
- `ReadStartupReport(out)`
- `Stop()`

## 2. libbpf Collector
`collector_libbpf.cpp` performs:
1. configure libbpf logging/strict mode
2. resolve BPF object path and open/load object
3. read runtime config (`config/default.yaml` or `ETRACEGEN_CONFIG`)
4. discover required maps and programs
5. apply runtime probe/domain toggles (`file`, `process`, `syscall`, `network_socket`)
6. attach BPF programs (best-effort)
7. create ring-buffer reader on `events`
8. poll and forward raw payloads to pipeline callback
9. aggregate `bpf_stats` counters
10. expose startup capability report

## 3. Decoder
`Decoder::Decode` converts raw wire payloads into typed variants:
- process
- file
- syscall
- network

## 4. Enricher
`enricher.cpp` is active, not a placeholder.
Current behavior is process-focused enrichment using `/proc` when available:
- executable path
- cwd
- cmdline
- parent comm
- start time ticks
- legacy filename compatibility for downstream consumers

This stage is intentionally kept in user space so the kernel payload stays small.

## 5. PolicyEngine
Current implementation is pass-through. Capture volume control is currently handled through domain toggles rather than allowlists.

## 6. JsonSink
`json_sink.cpp` writes NDJSON records to a file-backed sink, not stdout.
Current behavior:
- default path is `/tmp/etracegen/events_summary.ndjson`
- parent directories are created if needed
- new events are appended when the file exists
- file is rotated by delete-and-recreate when `max_file_size_bytes` is exceeded
- output is flushed periodically so external readers can consume it in near real time
- every emitted record includes a normalized `process_info` block built from a bounded userspace cache
- string fields are JSON-escaped before write

## 7. Metrics
Counters:
- received
- decoded
- dropped

## 8. Startup Report
Startup report provides backend/mode/attach/map visibility and degraded reason when capability is partial.
