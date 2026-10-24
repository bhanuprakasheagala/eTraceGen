# Pipeline Stages Walk

Sources:
- `../../src/enricher/enricher.cpp`
- `../../src/policy/policy.cpp`
- `../../src/sinks/json_sink.cpp`
- `../../src/metrics/metrics.h`

## Stage 1: Enricher
Current behavior: active process enrichment.
Source: `../../src/enricher/enricher.cpp:5`

What it currently does:
- resolves executable path from `/proc/<pid>/exe`
- resolves cwd from `/proc/<pid>/cwd`
- resolves cmdline from `/proc/<pid>/cmdline`
- resolves parent comm from `/proc/<ppid>/comm`
- reads process start time ticks from `/proc/<pid>/stat`
- keeps legacy `filename` populated for older consumers

Why it exists:
- keeps kernel payloads compact
- centralizes process-context enrichment in user space
- gives every event a consistent process identity block

Constraint:
- avoid unbounded latency in this stage.

## Stage 2: Policy Engine
Current behavior: always true.
Source: `../../src/policy/policy.cpp:5`

Intended role:
- filtering
- allow/deny rules
- sampling under pressure

Constraint:
- policy must be deterministic and cheap per event.

## Stage 3: Json Sink
Current behavior: file-backed NDJSON writer with rollover.
Source: `../../src/sinks/json_sink.cpp:519`

Behavior details:
- common header written first (`WriteHeader`)
- `std::visit` dispatches per concrete event type
- event-specific fields appended after the shared header and `process_info`
- output is written to `/tmp/etracegen/events_summary.ndjson` by default
- file is created if missing and appended to when present
- file rotates by delete-and-recreate once `max_file_size_bytes` is reached
- strings are JSON-escaped before serialization
- periodic flush keeps records visible to external readers without forcing a flush on every event

Potential caveat:
- the file sink is intentionally file-backed, so it is not a stdout pipeline.

## Metrics
Counters:
- received
- decoded
- dropped

Source: `../../src/metrics/metrics.h:8`

Current threading assumption:
- updated from callback thread in current design.
- if multi-threading is introduced later, counters should become atomic or synchronized.
