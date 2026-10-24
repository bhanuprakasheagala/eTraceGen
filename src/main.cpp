/*
 * File Notes:
 * - Process entry point and pipeline orchestrator.
 * - Owns lifecycle: startup, event loop, shutdown, and coarse health counters.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>

#include "collector/collector.h"
#include "decoder/decoder.h"
#include "metrics/metrics.h"
#include "model/normalizer.h"
#include "policy/policy.h"
#include "registry/entity_registry.h"
#include "sinks/json_sink.h"

namespace {
std::atomic<bool> g_running{true};

struct SinkRuntimeConfig {
  std::string path = "/var/log/etracegen/events.ndjson";
  uint64_t max_file_size_bytes = 100ULL * 1024ULL * 1024ULL;
};

/**
 * @brief Signal handler that requests graceful shutdown.
 *
 * @param sig Received POSIX signal number (unused).
 */
void OnSignal(int /*sig*/) { g_running = false; }

/**
 * @brief Trim leading and trailing ASCII whitespace.
 *
 * @param s Input string.
 * @return Whitespace-trimmed string copy.
 */
std::string Trim(std::string s) {
  const auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

/**
 * @brief Return runtime config path, optionally overridden by env var.
 */
std::string DefaultConfigPath() {
  const char* env_path = std::getenv("ETRACEGEN_CONFIG");
  return env_path ? env_path : "config/default.yaml";
}

/**
 * @brief Return true when event originates from this collector process.
 *
 * Intent:
 * Suppress collector self-events by pid/tgid so log writes do not re-enter
 * the telemetry stream.
 */
bool IsSelfEvent(const event_logger::EventVariant& event, uint32_t self_tgid) {
  // Match on tgid only: the collector's own events share its tgid. Matching on
  // pid too would over-broadly drop an unrelated thread whose TID equals our pid.
  return std::visit([self_tgid](const auto& ev) -> bool { return ev.hdr.tgid == self_tgid; },
                    event);
}

/**
 * @brief Parse sink settings from runtime config file.
 *
 * Parses a minimal subset of YAML-like keys under `sink:`:
 * - `path`
 * - `max_file_size_bytes`
 *
 * Parser is intentionally permissive and keeps defaults on parse errors.
 *
 * @return Resolved sink runtime configuration.
 */
SinkRuntimeConfig LoadSinkRuntimeConfig() {
  SinkRuntimeConfig cfg;

  std::ifstream in(DefaultConfigPath());
  if (!in.is_open()) {
    return cfg;
  }

  bool in_sink = false;
  std::string line;
  while (std::getline(in, line)) {
    const auto hash_pos = line.find('#');
    if (hash_pos != std::string::npos) {
      line = line.substr(0, hash_pos);
    }

    const std::string trimmed = Trim(line);
    if (trimmed.empty()) {
      continue;
    }

    if (trimmed == "sink:") {
      in_sink = true;
      continue;
    }

    if (trimmed.back() == ':' && trimmed != "sink:") {
      in_sink = false;
      continue;
    }

    if (!in_sink) {
      continue;
    }

    const auto colon = trimmed.find(':');
    if (colon == std::string::npos) {
      continue;
    }

    const std::string key = Trim(trimmed.substr(0, colon));
    std::string value = Trim(trimmed.substr(colon + 1));

    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                              (value.front() == '\'' && value.back() == '\''))) {
      value = value.substr(1, value.size() - 2);
    }

    if (key == "path") {
      cfg.path = value;
    } else if (key == "max_file_size_bytes") {
      try {
        cfg.max_file_size_bytes = std::stoull(value);
      } catch (...) {
        // Keep default when parse fails.
      }
    }
  }

  return cfg;
}

/**
 * @brief Emit one kernel-side diagnostics line to stderr.
 *
 * @param s Aggregated kernel BPF counters.
 * @param prefix Label used to distinguish startup vs periodic/final reports.
 */
void PrintKernelStats(const event_logger::KernelBpfStats& s, const char* prefix) {
  std::cerr << prefix << " ringbuf_reserve_fail=" << s.ringbuf_reserve_fail
            << " file_state_save_fail=" << s.file_state_save_fail
            << " file_state_collision=" << s.file_state_collision
            << " file_state_miss=" << s.file_state_miss
            << " syscall_state_save_fail=" << s.syscall_state_save_fail
            << " syscall_state_collision=" << s.syscall_state_collision
            << " syscall_state_miss=" << s.syscall_state_miss
            << " network_state_save_fail=" << s.network_state_save_fail
            << " network_state_collision=" << s.network_state_collision
            << " network_state_miss=" << s.network_state_miss << "\n";
}

/**
 * @brief Emit collector startup capability/degradation report.
 *
 * @param r Collector startup report produced by collector backend.
 */
void PrintStartupReport(const event_logger::CollectorStartupReport& r) {
  std::cerr << "startup_report backend=" << r.backend_name
            << " bpf_object_path=" << (r.bpf_object_path.empty() ? "n/a" : r.bpf_object_path)
            << " config_path=" << (r.config_path.empty() ? "n/a" : r.config_path)
            << " programs_total=" << r.programs_total
            << " programs_attached=" << r.programs_attached
            << " programs_attach_failed=" << r.programs_attach_failed
            << " runtime_config_loaded=" << (r.runtime_config_loaded ? "true" : "false")
            << " map_events_found=" << (r.map_events_found ? "true" : "false")
            << " map_bpf_stats_found=" << (r.map_bpf_stats_found ? "true" : "false")
            << " map_file_probe_enabled_found="
            << (r.map_file_probe_enabled_found ? "true" : "false")
            << " map_syscall_allowlist_found=" << (r.map_syscall_allowlist_found ? "true" : "false")
            << " map_pid_allowlist_found=" << (r.map_pid_allowlist_found ? "true" : "false")
            << " map_uid_allowlist_found=" << (r.map_uid_allowlist_found ? "true" : "false")
            << " map_network_probe_enabled_found="
            << (r.map_network_probe_enabled_found ? "true" : "false")
            << " map_network_port_allowlist_found="
            << (r.map_network_port_allowlist_found ? "true" : "false")
            << " map_network_port_filter_enabled_found="
            << (r.map_network_port_filter_enabled_found ? "true" : "false")
            << " map_suppress_tgid_found="
            << (r.map_suppress_tgid_found ? "true" : "false")
            << " file_probe_toggles_applied=" << (r.file_probe_toggles_applied ? "true" : "false")
            << " syscall_allowlist_applied_count=" << r.syscall_allowlist_applied_count
            << " pid_allowlist_applied_count=" << r.pid_allowlist_applied_count
            << " uid_allowlist_applied_count=" << r.uid_allowlist_applied_count
            << " network_probe_toggles_applied="
            << (r.network_probe_toggles_applied ? "true" : "false")
            << " network_port_allowlist_applied_count=" << r.network_port_allowlist_applied_count
            << " degraded=" << (r.degraded ? "true" : "false");

  if (!r.degrade_reason.empty()) {
    std::cerr << " degrade_reason=\"" << r.degrade_reason << "\"";
  }
  std::cerr << "\n";
}

/**
 * @brief Build the file-backed JSON sink from config defaults/overrides.
 *
 * @return Initialized sink instance (may be not-ready when open fails).
 */
std::unique_ptr<event_logger::JsonSink> BuildSink() {
  const SinkRuntimeConfig cfg = LoadSinkRuntimeConfig();
  auto sink = std::make_unique<event_logger::JsonSink>(cfg.path, cfg.max_file_size_bytes);

  std::cerr << "[sink] file_json path=" << cfg.path
            << " max_file_size_bytes=" << cfg.max_file_size_bytes << "\n";

  return sink;
}
}  // namespace

int main() {
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);
  const uint32_t self_tgid = static_cast<uint32_t>(::getpid());

  auto* collector = event_logger::CreateCollector();
  event_logger::Decoder decoder;
  event_logger::Normalizer normalizer;
  event_logger::EntityRegistry registry;
  event_logger::PolicyEngine policy;
  auto sink = BuildSink();
  if (!sink || !sink->IsReady()) {
    std::cerr << "failed to initialize file sink; check sink.path permissions\n";
    return 1;
  }

  event_logger::Metrics metrics;

  // Raw callback is the top of the userspace event path:
  // bytes -> decode -> self-filter -> normalize -> registry -> policy -> sink.
  const bool started = collector->Start([&](std::span<const unsigned char> raw) {
    metrics.IncrementReceived();

    auto decoded = decoder.Decode(raw);
    if (!decoded.has_value()) {
      metrics.IncrementDropped();
      return;
    }
    metrics.IncrementDecoded();
    auto event = *decoded;

    if (IsSelfEvent(event, self_tgid)) {
      return;
    }

    // Normalize into the canonical model and update the entity registry from the
    // full fact stream. This runs before policy so registry knowledge stays
    // complete even for events policy later suppresses from the sink. The
    // registry is the single source of process context (formerly the enricher).
    event_logger::CanonicalEvent canonical = normalizer.Normalize(event);
    registry.Ingest(canonical);

    if (!policy.Allow(event)) {
      return;
    }

    sink->Write(canonical);
  });

  event_logger::CollectorStartupReport startup_report{};
  if (collector->ReadStartupReport(&startup_report)) {
    PrintStartupReport(startup_report);
  }

  if (!started) {
    std::cerr << "failed to start collector\n";
    return 1;
  }

  using clock = std::chrono::steady_clock;
  auto next_stats_report = clock::now() + std::chrono::seconds(30);

  // Main loop keeps ingestion responsive and emits periodic kernel diagnostics.
  while (g_running) {
    collector->PollOnce(200);
    sink->FlushDue();

    const auto now = clock::now();
    if (now >= next_stats_report) {
      event_logger::KernelBpfStats periodic_stats{};
      if (collector->ReadKernelBpfStats(&periodic_stats)) {
        PrintKernelStats(periodic_stats, "kernel_stats_periodic");
      }
      next_stats_report = now + std::chrono::seconds(30);
    }
  }

  event_logger::KernelBpfStats kernel_stats{};
  const bool has_kernel_stats = collector->ReadKernelBpfStats(&kernel_stats);

  collector->Stop();
  sink->FlushPending();

  std::cerr << "received=" << metrics.received() << " decoded=" << metrics.decoded()
            << " dropped=" << metrics.dropped() << "\n";

  if (has_kernel_stats) {
    PrintKernelStats(kernel_stats, "kernel_stats_final");
  }

  return 0;
}
