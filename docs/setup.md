# Linux Setup, Build, and Bring-up

This guide is the post-clone path for building eTraceGen, running it on Linux, and seeing telemetry
output. It also captures build/portability and preflight/validation notes.

eTraceGen is **Linux-only**. `CMakeLists.txt` fails configure on non-Linux hosts and requires
`libbpf` via `pkg-config`.

## 1. Compatibility Baseline
Recommended baseline:
- kernel: `>= 5.15`
- architecture: `x86_64` first, then `arm64`
- BTF available: `/sys/kernel/btf/vmlinux`
- runtime privilege: root for initial validation

Required kernel capabilities/config:
- `CONFIG_BPF=y`
- `CONFIG_BPF_SYSCALL=y`
- `CONFIG_BPF_JIT=y`
- tracepoint support

Runtime compatibility depends on kernel version/features, distro kernel config, security controls
(`cap_bpf`, lockdown, LSM), and BTF availability.

## 2. Install Dependencies

### Debian / Ubuntu
```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build gcc g++ clang llvm pkg-config \
  libelf-dev zlib1g-dev libbpf-dev bpftool pahole linux-headers-$(uname -r)
```

### Fedora / RHEL / Rocky / AlmaLinux
```bash
sudo dnf install -y gcc-c++ make cmake ninja-build clang llvm pkgconf-pkg-config \
  elfutils-libelf-devel zlib-devel libbpf libbpf-devel bpftool dwarves \
  kernel-devel kernel-headers
```

### Arch Linux
```bash
sudo pacman -S --needed base-devel cmake ninja clang llvm pkgconf \
  libelf zlib libbpf bpftool pahole linux-headers
```

## 3. Build
```bash
git clone <your-repo-url>
cd eTraceGen
./scripts/linux.sh all      # builds userspace binary + BPF object
```

Outputs:
- `build/etracegen`
- `bpf/event_logger.bpf.o`

Individual steps:
- `./scripts/linux.sh build` — userspace binary
- `./scripts/linux.sh bpf` — BPF object (generates `bpf/vmlinux.h` from host BTF)

### BPF object resolution at runtime
Order of precedence:
1. `ETRACEGEN_BPF_OBJECT` environment variable
2. build-time default path (`EVENT_LOGGER_DEFAULT_BPF_OBJECT`, injected by CMake)

The build-time default makes the runtime independent of the current working directory.

## 4. Host Check and Preflight
```bash
./scripts/linux.sh check       # host prerequisites (tools, BTF, kernel config)
./scripts/linux.sh preflight   # structural + operational release gate
```

Preflight verifies: required source/config/doc files exist, startup-report wiring is present, the
raw syscall handler exists in the kernel program, and that `build/etracegen` + `bpf/event_logger.bpf.o`
are built, then runs the host check.

## 5. Run and Observe
```bash
sudo ./scripts/linux.sh run
```

Generate activity in another terminal:
```bash
ls /tmp >/dev/null
cat /etc/hosts >/dev/null
echo hi >/tmp/etracegen_demo.txt
rm -f /tmp/etracegen_demo.txt
```

## 6. Validate
```bash
./scripts/linux.sh smoke      # integration smoke test
./scripts/linux.sh validate   # v1 validation suite (build/validate_v1/*.ndjson)
./scripts/linux.sh verify     # build + bpf + preflight + smoke + validate
```

The validation suite covers a `baseline_all` case (process/file/syscall enabled) and a
`syscall_domain_off` case (verifies `domains.syscall: false` disables syscall output). It is
functional validation, not performance benchmarking.

## 7. Runtime Configuration
Behavior is controlled by `config/default.yaml` (or the path in `ETRACEGEN_CONFIG`).

Current high-level controls are **domain toggles**:
- `domains.process`
- `domains.file`
- `domains.syscall`
- `domains.network_socket`

Network socket capture is enabled by default. Successful socket I/O is summarized by process,
socket, direction, transport, and endpoint every five seconds; routine nonblocking retries and
socket option/name lookups are suppressed. Set `domains.network_socket: false` to disable it.

Current mode is **capture-first**: allowlist/blocklist filtering (PID/UID/syscall/port) is present
in the maps but disabled in the active flow. Domain toggles are the primary runtime controls. The
planned signal-first policy profiles are described in [roadmap.md](roadmap.md).

## 8. Log Sink Notes
- Default sink is file-based NDJSON at `/tmp/etracegen/events_summary.ndjson` (override with
  `sink.path`).
- Parent directories are created if missing; the collector appends when the file exists.
- The summary filename keeps new compact records separate from older per-syscall logs.
- When `sink.max_file_size_bytes` is reached, the collector deletes and recreates the file.
- Output is flushed periodically so external readers can consume it in near real time.

## 9. Troubleshooting
Common failure classes:
- repeated `comm:"etracegen"` + very high CPU indicates a self-event feedback loop; rebuild the BPF
  object (`./scripts/linux.sh all`) so the `suppress_tgid` map is present.
- one failed attach (e.g. `syscalls/sys_exit_vfork`) can be kernel-variant behavior; the collector
  continues in degraded mode if other probes attach.
- missing BTF (`/sys/kernel/btf/vmlinux` absent)
- missing/mismatched kernel headers
- kernel too old for expected helper/tracepoint behavior
- security policy/lockdown blocking BPF load/attach
- insufficient privileges/capabilities

First triage commands:
```bash
uname -a
bpftool version
ls -l /sys/kernel/btf/vmlinux
zgrep -E 'CONFIG_BPF|CONFIG_BPF_SYSCALL|CONFIG_BPF_JIT' /proc/config.gz
```

## 10. process_info Validation
After a `run`, verify unified process context is present on every record:
```bash
# should print 0 when every record has process_info
jq -c "select(.process_info == null)" /tmp/etracegen/events_summary.ndjson | wc -l

# inspect enriched process fields
head -n 20 /tmp/etracegen/events_summary.ndjson | jq ".process_info"
```
