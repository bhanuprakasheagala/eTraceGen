# Verification Checklist (Linux)

A top-to-bottom guide for building, running, and **verifying** eTraceGen on a Linux host after code
changes. Work through it in order — each phase gates the next. Every check lists **what to run**,
**what "good" looks like**, **why it matters**, and **what to do if it fails**.

> This project is Linux-only and cannot be built or run on Windows. Run everything below on the
> Linux target (kernel ≥ 5.15, BTF at `/sys/kernel/btf/vmlinux`, root for attach). See
> [setup.md](setup.md) for dependency install.

Legend: check the box when a step passes. Tags like `[H2]`, `[M3]`, `[Stage 3]` link a check to the
change it protects.

---

## 0. Quick sign-off gate (the short version)

If you only have five minutes, these are the must-pass gates. Details for each are below.

| # | Gate | Command | Pass condition |
|---|------|---------|----------------|
| 1 | Configure + build | `./scripts/linux.sh build` | exits 0, `build/etracegen` exists |
| 2 | BPF object builds | `./scripts/linux.sh bpf` | exits 0, `bpf/event_logger.bpf.o` exists |
| 3 | Unit tests | `./scripts/linux.sh test` | `policy_process` + `model_registry` pass |
| 4 | Preflight | `./scripts/linux.sh preflight` | "release preflight passed" |
| 5 | Smoke | `./scripts/linux.sh smoke` | "integration smoke test succeeded" |
| 6 | Validate | `./scripts/linux.sh validate` | "v1 validation suite passed" |
| 7 | Live run | `sudo ./scripts/linux.sh run` + activity | events land in the sink file with `process_info` + `entity_id` |
| 8 | No self-noise | grep sink for `etracegen` | collector does not log itself |

One-shot equivalent: `./scripts/linux.sh verify` (runs build + bpf + preflight + test + smoke +
validate). Then do the **live run** section manually.

---

## 1. Environment preconditions

- [ ] **Host check** — `./scripts/linux.sh check`
  - Good: all `[pass]` for gcc/g++/cmake/pkg-config/clang/llvm-strip/bpftool/pahole and
    `kernel BTF present`. Root recommended.
  - Why: missing BTF or toolchain is the most common cause of a failed BPF build/attach.
  - If it fails: install the packages listed in the hint line / [setup.md](setup.md).
- [ ] **Kernel config** — `zgrep -E 'CONFIG_BPF|CONFIG_BPF_SYSCALL|CONFIG_BPF_JIT' /proc/config.gz`
  - Good: all `=y`. Why: required for load/attach.

---

## 2. Build (userspace)

- [ ] **Configure + compile** — `./scripts/linux.sh build`
  - Good: CMake configures with **no missing-source error**, compiles with **zero warnings**
    (`-Wall -Wextra -Wpedantic`), produces `build/etracegen`.
  - Why `[M0]`: the `tests/` target used to be missing and broke configure. It must configure clean.
  - Watch for: warnings in the new modules (`src/model/`, `src/registry/`, `src/util/`) or the sink —
    treat any warning as a defect to fix, not ignore.
- [ ] **New translation units are actually compiled** — check the build log mentions
  `normalizer.cpp`, `entity_registry.cpp`, `proc_util.cpp`.
  - Why `[H2]`: consolidation only holds if these are in the build and the enricher is gone
    (`src/enricher/` should not exist).

## 3. Build (eBPF object)

- [ ] **BPF compile** — `./scripts/linux.sh bpf`
  - Good: `built bpf/event_logger.bpf.o`. Regenerates `bpf/vmlinux.h` from host BTF.
  - Why `[M3]`: the exec handler now reads `ctx->__data_loc_filename` from
    `trace_event_raw_sched_process_exec`. If the compile errors on that field/struct name, the
    `vmlinux.h` for this kernel names it differently — report the exact error before proceeding.
  - Note: the network module is now part of this object. The **key gate is load-time verification**
    (`./scripts/linux.sh run`): the 23 enter handlers stage their ~512 B payload in the per-CPU
    `network_scratch` map, not on the stack. If `bpf_object__load` reports a stack-size/verifier
    rejection naming a `on_sys_enter_*` network program, report which one.

---

## 4. Unit tests

- [ ] **Run tests** — `./scripts/linux.sh test` (or `ctest --test-dir build --output-on-failure`)
  - Good: `policy_process` and `model_registry` both report `OK` / pass.
  - Why `[M1][M2]`: `model_registry` covers the tricky stateful logic — canonical mapping, stable
    identity, **exit-based reuse**, **lineage**, and **bounded eviction**. A failure here means the
    knowledge core is wrong; fix before running live.
  - If `model_registry` fails: read which `[FAIL]` line printed; the test names map directly to the
    invariant that broke.

---

## 5. Preflight + automated suites

- [ ] **Preflight** — `./scripts/linux.sh preflight`
  - Good: "release preflight passed". Verifies required files exist (incl. `docs/setup.md`), the
    startup-report wiring, the raw syscall handler, and that both binary + BPF object are built.
- [ ] **Smoke** — `./scripts/linux.sh smoke`
  - Good: "integration smoke test succeeded".
  - Why `[H1]`: this used to grep an empty stdout capture and always fail. It now writes a config
    with an explicit `sink.path` (`build/integration_smoke_events.ndjson`) and greps **that file**
    for `process`, `file`, and `syscall` events.
  - If it fails: inspect `build/integration_smoke_stderr.log` (startup report / degrade reasons) and
    `build/integration_smoke_events.ndjson` (did any events land?).
- [ ] **Validate** — `./scripts/linux.sh validate`
  - Good: "v1 validation suite passed". Two cases: `baseline_all` (syscall on) and
    `syscall_domain_off` (asserts **no** syscall events when `domains.syscall: false`).
  - Why: proves the domain toggles actually gate output, reading the real sink file per case
    (`build/validate_v1/*.ndjson`).

---

## 6. Live run and output correctness

Start the collector, generate activity in a second terminal, stop with Ctrl-C.

```bash
# terminal 1
sudo ./scripts/linux.sh run            # uses config/default.yaml -> summary sink

# terminal 2 — generate a mix of activity
ls /tmp >/dev/null
cat /etc/hostname >/dev/null
echo hi >/tmp/etg_demo.txt && rm -f /tmp/etg_demo.txt
for i in $(seq 1 50); do /bin/true; done     # short-lived processes (for the M3 check)
```

Let `SINK=/tmp/etracegen/events_summary.ndjson` for the checks below.

### 6.1 Startup health
- [ ] **Startup report** — read terminal 1 stderr, line `startup_report ...`.
  - Good: `programs_attached` > 0, `map_events_found=true`, `degraded=false`.
  - Acceptable degradation: one or two `programs_attach_failed` (e.g. `sys_exit_vfork` on some
    kernels) with the collector still running — `degrade_reason` explains it.
  - Bad: `programs_attached=0` or `events map missing` → nothing will be captured.
- [ ] **Sink file exists and grows** — `wc -l "$SINK"` while activity runs.
  - Good: line count increases. Why `[H1]`: confirms events reach the **file** sink (not stdout).

### 6.2 Every record is well-formed and has knowledge-layer identity
- [ ] **Valid JSON, one object per line** — `jq -c . "$SINK" | tail -n 3`
  - Good: parses without error.
- [ ] **`process_info` present on every record** `[H2]` —
  `jq -c 'select(.process_info == null)' "$SINK" | wc -l`
  - Good: **0**. Every event carries the normalized `process_info` block.
- [ ] **`entity_id` present and non-zero** `[Stage 3]` —
  `jq -c 'select((.entity_id // 0) == 0)' "$SINK" | wc -l`
  - Good: **0** (or only rare records with `tgid==0`). Why: the registry resolved every actor.
- [ ] **Spot-check a process record** — `jq 'select(.type=="process")' "$SINK" | head`
  - Good: has `entity_id`, and `process_info` with populated `comm`; `exec_path`/`cmdline` populated
    for most live processes.

### 6.3 Registry correctness (the knowledge layer)
- [ ] **Stable identity — same process keeps one `entity_id`** `[Stage 2]`
  ```bash
  # pick a busy pid's tgid, confirm all its records share one entity_id
  jq -r 'select(.type=="file") | "\(.tgid) \(.entity_id)"' "$SINK" | sort -u | head
  ```
  - Good: each `tgid` (of a still-living process) maps to exactly **one** `entity_id`.
- [ ] **Distinct processes get distinct ids** — different tgids show different `entity_id`s.
- [ ] **Lineage on process creation** `[Stage 2]` —
  `jq -c 'select(.type=="process" and (.object_entity_id != null)) | {kind, entity_id, object_entity_id}' "$SINK" | head`
  - Good: fork/clone/vfork records (`kind` 2/4/5/6) carry an `object_entity_id` (the child entity),
    distinct from the parent `entity_id`.
  - Note: parent→child linkage is tracked internally (`parent_id`); only `entity_id` /
    `object_entity_id` are emitted today.

### 6.4 Exec path reliability for short-lived processes `[M3]`
- [ ] After the `/bin/true` loop above, check exec records resolved a path:
  ```bash
  jq -r 'select(.type=="process" and .kind==1) | .process_info.exec_path' "$SINK" | sort | uniq -c
  ```
  - Good: `/usr/bin/true` (or `/bin/true`) appears for the loop — **not** empty strings. This is the
    in-kernel `filename` capture doing its job when `/proc` is already gone.
  - If many are empty: the kernel `__data_loc_filename` read may be wrong for this kernel — revisit
    the M3 change.

### 6.5 Self-suppression `[Stage 1 / policy]`
- [ ] **Collector does not log itself** — `grep -c '"comm":"etracegen"' "$SINK"`
  - Good: **0**. Why: prevents a self-feedback loop. If non-zero *and* CPU is pegged, the
    `suppress_tgid` map is likely missing — rebuild the BPF object (`./scripts/linux.sh all`).

### 6.6 Domain toggles behave
- [ ] With default config, `jq -r .type "$SINK" | sort | uniq -c` shows `process`, `file`, and
  `network` but **no `syscall`**.
- [ ] Generate traffic (`curl`, `getent hosts`, `nc`) and confirm network lifecycle records and
  periodic `io_summary` records include endpoints, operation counts, and transferred bytes.
  Routine nonblocking retries and socket option/name lookups should not appear.
- [ ] **Non-socket close is not a network event** — after touching/reading a regular file, confirm no
  `network` event with `"event_name":"close"` for that fd. Why: `close` gates on `is_socket_fd()`.

---

## 7. Rotation / truncation behavior `[H4]`

- [ ] Point the sink at a tiny cap and confirm **truncation** (delete-and-recreate), understanding it
  **loses history** at the boundary:
  ```bash
  mkdir -p /tmp/etg && cat > /tmp/etg.yaml <<'CFG'
  domains: { process: true, file: true, syscall: true }
  sink: { path: /tmp/etg/events.ndjson, max_file_size_bytes: 65536 }
  CFG
  sudo ETRACEGEN_CONFIG=/tmp/etg.yaml ETRACEGEN_BPF_OBJECT=bpf/event_logger.bpf.o ./build/etracegen &
  # generate activity, watch the file size cap and reset:
  watch -n1 'ls -l /tmp/etg/events.ndjson'
  ```
  - Good: file grows to ~64 KiB then **resets to ~0** (no `.1` backup appears). This confirms the
    documented truncation semantics — decide whether that data-loss profile is acceptable for your
    use before relying on it.

---

## 8. Overhead & kernel health

- [ ] **Kernel drop/correlation stats** — read the periodic `kernel_stats_periodic ...` line (every
  30 s) and the `kernel_stats_final ...` line on shutdown.
  - Good: `ringbuf_reserve_fail` stays low/zero under normal load; `*_state_miss` not growing
    unboundedly.
  - Rising `ringbuf_reserve_fail`: userspace isn't draining fast enough or event volume is too high
    (expected if you enable `syscall`) — the ring buffer is 16 MiB; sustained pressure is a signal to
    reduce captured domains.
- [ ] **CPU** — `top -p "$(pgrep -x etracegen)"`.
  - Good: modest, stable CPU. A pegged core + `etracegen` self-events = feedback loop (see 6.5).
- [ ] **Pipeline counters** on shutdown — `received= decoded= dropped=` line.
  - Good: `dropped` (decode failures) near 0. Note `[M5]`: `decoded` counts events that passed
    decode, **not** records written — self/policy-suppressed events aren't separately counted.

---

## 9. Known-good "do not be alarmed" list

These are intentional current behaviors, not bugs (see [event-contract.md](event-contract.md) §7):

- `syscall` records show `is_enter: 0` always (only the paired exit is emitted).
- `network_*` fields `bytes_captured` / `bytes_truncated` are always `0` (no payload capture).
- Socket I/O (`read`/`write`/`readv`/`writev`/`sendmmsg`/`recvmmsg`/`close`) is captured only for
  fds observed via `socket`/`accept`/`accept4`/`socketpair` after startup; pre-existing or
  `SCM_RIGHTS`/`dup`-received socket fds are not recognized (intentional gate).
- `process_info.exec_path`/`cmdline` may be empty for a process that vanished before enrichment and
  had no exec event (exit-only visibility).
- Endpoint `*_addr` hex is **network** byte order while `*_port` is **host** order.

---

## 10. Reliability direction — deeper confidence checks

Beyond "it runs," these build trust that we're on a reliable-product path:

- [ ] **Determinism across runs:** run twice with identical activity; `type` distribution and
  `process_info` shape should be stable (entity_id values differ between runs — they're per-run).
- [ ] **PID-reuse safety (best-effort):** on a busy host over a longer run, confirm you don't see a
  single `entity_id` attached to two obviously different `comm`/`exec_path` values — that would mean
  reuse detection missed a boundary.
- [ ] **Long-run stability:** let it run 30–60 min under load; memory should plateau (registry caps
  at 8192 entities, LRU-evicted) and `ringbuf_reserve_fail` should not climb steadily.
- [ ] **Graceful shutdown:** Ctrl-C prints final stats and exits 0; the sink file is flushed and
  valid to the last line (`jq -c . "$SINK" | tail`).
- [ ] **Degraded-mode honesty:** if you deliberately run without root, the startup report should say
  `degraded=true` with a clear `degrade_reason`, not silently produce nothing.

---

## 11. If something fails

1. Capture the exact command, its stderr, and the `startup_report` / `kernel_stats_*` lines.
2. For build errors in `src/model|registry|util` → likely an include or type mismatch from the
   knowledge-layer changes.
3. For BPF compile errors → almost always a `vmlinux.h` / kernel-version mismatch (esp. the M3
   `__data_loc_filename` read).
4. For "no events" → check the startup report (`programs_attached`, `map_events_found`) and that you
   ran as root.
5. See the troubleshooting section in [setup.md](setup.md) for common failure classes.
