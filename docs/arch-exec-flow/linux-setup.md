# Linux Host Setup and Bring-up

This guide is the post-clone path for running eTraceGen on Linux and seeing real-time telemetry output.

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

## 2. Install Dependencies

### Debian / Ubuntu
```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build gcc g++ clang llvm pkg-config \
  libelf-dev zlib1g-dev libbpf-dev bpftool pahole linux-headers-$(uname -r)
```

### Fedora
```bash
sudo dnf install -y gcc-c++ make cmake ninja-build clang llvm pkgconf-pkg-config \
  elfutils-libelf-devel zlib-devel libbpf libbpf-devel bpftool dwarves \
  kernel-devel kernel-headers
```

### RHEL / Rocky / AlmaLinux
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
./scripts/linux.sh all
```

## 4. Host Check and Preflight
```bash
./scripts/linux.sh check
./scripts/linux.sh preflight
```

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
(exec 3<>/dev/tcp/127.0.0.1/1) >/dev/null 2>&1 || true
```

## 6. Validate
```bash
./scripts/linux.sh smoke
./scripts/linux.sh validate
```

Full sequence:
```bash
./scripts/linux.sh verify
```

## 7. Log Sink Notes
- default sink is file-based NDJSON at `/tmp/etracegen/events_summary.ndjson`
- collector appends when file exists
- when `sink.max_file_size_bytes` threshold is reached, collector deletes and recreates the file

## 8. Capture-First Runtime Notes
- default config enables process/file/network domains; syscall capture is opt-in and network I/O is summarized
- runtime allowlist/blocklist filtering is disabled in active flow
- domain toggles remain the primary controls

## 9. Troubleshooting
Common failure classes:
- repeated `comm:"etracegen"` + very high CPU indicates self-event feedback loop; rebuild BPF object and rerun (`./scripts/linux.sh all`) so `suppress_tgid` map is present
- one failed attach like `syscalls/sys_exit_vfork` can be kernel-variant behavior; collector continues in degraded mode if other probes attach
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
Use these checks after `run` to verify unified process context is present:

```bash
# should print 0 when every record has process_info
jq -c "select(.process_info == null)" /tmp/etracegen/events_summary.ndjson | wc -l

# inspect enriched process fields
head -n 20 /tmp/etracegen/events_summary.ndjson | jq ".process_info"
```
