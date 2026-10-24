# eTraceGen Roadmap

This is the single, current roadmap. It supersedes all earlier planning notes and consolidates
them into one realistic, sequenced path.

## The goal

> **From Kernel Observations to System Intelligence.**

eTraceGen ingests the **whole stream of kernel facts** and progressively transforms it into
reusable **system knowledge**, and ultimately **system intelligence**. The distinction that drives
every decision:

- **Raw kernel facts are the *substrate*, not the product.** We keep capturing everything the kernel
  can tell us; we do not narrow to a security-filtered subset.
- **The product is the transformation layer** that turns those facts into stateful, correlated,
  meaningful knowledge — the README's Knowledge/Intelligence stack:
  `Canonical Event Model → Entity Registry → Correlation Engine → Semantic Engine → Policy Engine`.

> **On "the logs are too noisy":** that is a *symptom of the knowledge layers not existing yet* — we
> are emitting the raw substrate as if it were the product. The fix is not to capture less; it is to
> build the layers that produce intelligence, and let the raw stream remain an internal substrate.

### Direction decisions (confirmed)
- **Scope:** general-purpose *system intelligence*, not a security/EDR tool. (The malware-analysis
  lab was the project's origin, not its destination.)
- **First real work:** build the **Canonical Event Model and Entity Registry together** as one
  foundation milestone.
- **Final intelligence surface** (knowledge graph vs. timelines vs. behavioral insights): **left open
  on purpose.** Build the substrate well first; choose the surface once the knowledge layers exist.

## Where we are today
- eBPF captures process/file/network facts by default; syscall capture is opt-in. Successful
  network I/O is summarized in userspace to keep the log stream manageable (see the network track below).
- Facts are decoded to typed variants and serialized to NDJSON.
- There is a **seed of an entity registry**: the `ProcessInfoCache` in
  [`src/sinks/json_sink.cpp`](../src/sinks/json_sink.cpp) already maintains per-process state with
  TTL and PID-reuse guarding. That logic is the natural nucleus of the real registry.
- No canonical model, no correlation, no semantic layer. `PolicyEngine` is a pass-through.

## Principles
- Ingest the whole fact stream; never drop facts the knowledge layers may need. Keep a complete raw
  substrate channel available.
- Kernel stays thin and verifier-friendly; **all knowledge construction happens in user space.**
- The **canonical model decouples** the knowledge layers from kernel-specific event shapes and from
  the wire ABI ([event-contract.md](event-contract.md)), so the kernel can evolve without breaking
  intelligence code.
- Each layer is additive and independently testable.
- Keep the final intelligence surface **pluggable** until we commit to one.

---

## Milestone 0 — Unblock the build (prerequisite, small)

**Why:** `CMakeLists.txt` builds `test_policy_process` from `tests/test_policy_process.cpp`, but the
`tests/` directory doesn't exist, so a clean configure fails. We are about to add stateful knowledge
components that need tests.

**Work**
- Add a minimal real `tests/test_policy_process.cpp` (a scaffold to grow into registry/correlation
  tests), or gate the target behind an option. Preference: add the test.
- Confirm `./scripts/linux.sh all` + `preflight` pass on a Linux host.

**Acceptance:** `cmake -S . -B build` configures/builds cleanly (incl. tests); `verify` runs without
a configure/build failure.

---

## Milestone 1 — Canonical Event Model + Entity Registry (the foundation) — LEAD

The keystone. Build the normalized representation and the stateful entity store **together**, since
the registry populates and is keyed by the canonical model.

### 1.1 Canonical Event Model
- Define a stable, kernel-agnostic internal representation (e.g. `CanonicalEvent`) produced by a
  normalization stage **after decode/enrich**: identity/time, a resolved **entity reference**, an
  `action`, and a `subject/object/outcome` shape where applicable.
- Downstream layers consume the canonical model, never raw kernel structs. The raw typed event
  remains available as the substrate.
- **Where:** a new module (e.g. `src/model/`) between the decode/enrich stages and the sink; reuse
  the existing enrichment in [`src/enricher/enricher.cpp`](../src/enricher/enricher.cpp).

### 1.2 Entity Registry
- A first-class, stateful store of system **entities**, updated from every canonical event:
  - **Process** entities: stable entity ID, pid/tgid, **lineage** (parent/child), lifecycle
    (start/exit), identity (exec_path, cmdline, start_time). Promote the `ProcessInfoCache` logic out
    of the sink into this registry.
  - **File** entities: path (and inode identity where available), lifecycle.
  - **Socket** entities: fd / `flow_id` identity — ready for when network facts are enabled.
- Reuse the existing **PID-reuse guarding** (start-time comparison) already in `json_sink.cpp`.
- **Where:** a new module (e.g. `src/registry/`); the sink and later layers query it instead of
  rebuilding process context.

**Acceptance criteria**
- Every event is normalized to a canonical form carrying a **stable entity reference**.
- The registry reflects the live process tree (lineage + lifecycle) and survives PID reuse.
- Entities are queryable in-process; the sink sources process context from the registry (removing
  the duplicated cache).
- Unit tests cover normalization and registry lifecycle/reuse (building on M0).

---

## Milestone 2 — Correlation Engine

Turn independent entity facts into **relationships across entities and time**.

**Work**
- Link canonical events to entities and to each other: process→file (opened/created/deleted),
  process→socket, parent→child spawn, and lifecycle chains.
- Emit **correlated records** (entity refs + relationships), not isolated events.
- Maintain bounded correlation state (windows / entity lifetimes) to stay memory-safe.

**Acceptance criteria**
- For a given workload, the system can reconstruct "which process touched which file/socket" and the
  parent/child spawn graph from registry + correlation state.
- Correlation state is bounded and does not grow unboundedly under load.

---

## Milestone 3 — Semantic Engine + intelligence surface (surface deferred)

Where correlated knowledge becomes **meaning** — and where the "raw → meaningful" transformation
happens as a *product of knowledge construction*, not event suppression.

**Work**
- Infer higher-level behaviors from correlated state (semantic events with clear
  subject/action/object/outcome). Design references (framing is illustrative, not security-specific):
  [signal-first-design.md](signal-first-design.md), [policy-and-semantics.md](policy-and-semantics.md).
- The **Policy Engine becomes a consumer/selector** over the layers: emit intelligence by default,
  keep the raw substrate available on demand (profiles). This is where noise finally resolves —
  the default output is knowledge, not raw facts.
- **Choose the final intelligence surface here** (knowledge graph / activity timelines / behavioral
  insights) once the substrate is proven. Kept pluggable until then.

**Acceptance criteria** (to be refined once M1–M2 land)
- Correlated activity is expressible as a small set of semantic events.
- Default output is intelligence; the complete raw substrate is still retrievable for debugging.

---

## Cross-cutting tracks (scheduled into the milestones above)

### Network facts (more substrate) — DONE
The network module (`bpf/event_logger_network.bpf.c`) is **compiled into the BPF object and enabled
by default** (see [architecture.md](architecture.md) §5). It adds socket facts to the substrate; the
normalizer/registry already consume `network_event` like every other domain. What remains is wiring
socket facts into **socket entities** and correlation (M2).
- Done: `#include "event_logger_network.bpf.c"` in the aggregator; enter handlers stage their payload
  in a per-CPU scratch map (`network_scratch`) to stay under the verifier's 512-byte stack limit;
  `close` gates on `is_socket_fd()` so regular-file closes are not mislabeled as network events.
- Still substrate-only: socket **entities** in the registry, and process→socket correlation, land in M2.
- Risk notes (live): some syscall tracepoints (`sendmmsg`/`recvmmsg`) may be absent on some kernels
  (best-effort attach already degrades); `read`/`write`/`close` hooks gate on `is_socket_fd()` and
  miss socket fds opened before startup; watch `bpf_stats` reserve failures under the high-volume
  "everything on" profile.

### Output & noise
Raw NDJSON stays as the substrate channel throughout. The "too noisy" experience resolves at M3 when
the sink emits canonical/semantic records by default. No aggressive early event-dropping is needed —
completeness of the substrate is a requirement of the knowledge layers.

### ABI stability
The wire schema ([event-contract.md](event-contract.md)) is unchanged as the transport layer; the
canonical model is a user-space layer above it.

### Known tech debt (address opportunistically)
- **Hand-rolled YAML parsing is duplicated** across `src/main.cpp` (sink block) and
  `src/collector/collector_libbpf.cpp` (domains/probes/filters), and is brittle: section detection
  via a trailing `:` misfires on values ending in `:`, and the `*_allowlist` keys are matched by
  prefix anywhere rather than gated to their section. Consolidate onto one small shared parser (or a
  real YAML library) when config handling is next touched.
- **Metrics don't reconcile:** suppressed (self/policy) events are not counted; add explicit
  suppression counters alongside received/decoded/dropped.

---

## Sequencing summary

| Milestone | Focus | Gate |
|-----------|-------|------|
| M0 | Unblock build (missing test target) | clean `verify` |
| M1 | Canonical Event Model + Entity Registry (lead) | events normalized to stable entity refs; live process tree |
| M2 | Correlation Engine | reconstruct entity relationships from state |
| M3 | Semantic Engine + intelligence surface (surface TBD) | default output is knowledge, raw substrate retained |
| — | Network facts / output profiles | folded into M1–M3 as needed |
