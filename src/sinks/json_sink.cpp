/*
 * File Notes:
 * - Serializes typed events into compact JSON lines for ingestion.
 * - File-backed output with optional max-size rollover.
 * - Aggregates successful socket I/O and includes resolved process context where useful.
 */

#include "sinks/json_sink.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

#include <sys/socket.h>

#include "registry/entity_registry.h"

namespace event_logger {
namespace {

/**
 *  Return steady-clock timestamp in nanoseconds for flush scheduling.
 */
uint64_t NowMonoNs() {
  using clock = std::chrono::steady_clock;
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count());
}

/**
 *  Convert numeric event family into stable JSON type label.
 */
const char* EventTypeToString(uint32_t type) {
  switch (type) {
    case EVENT_TYPE_PROCESS:
      return "process";
    case EVENT_TYPE_FILE:
      return "file";
    case EVENT_TYPE_SYSCALL:
      return "syscall";
    case EVENT_TYPE_NETWORK:
      return "network";
    default:
      return "unknown";
  }
}

const char* NetworkEventKindToString(uint32_t kind) {
  switch (kind) {
    case NETWORK_SOCKET:
      return "socket";
    case NETWORK_CONNECT:
      return "connect";
    case NETWORK_ACCEPT:
      return "accept";
    case NETWORK_BIND:
      return "bind";
    case NETWORK_LISTEN:
      return "listen";
    case NETWORK_CLOSE:
      return "close";
    case NETWORK_SENDTO:
      return "sendto";
    case NETWORK_RECVFROM:
      return "recvfrom";
    case NETWORK_SHUTDOWN:
      return "shutdown";
    case NETWORK_SOCKETPAIR:
      return "socketpair";
    case NETWORK_ACCEPT4:
      return "accept4";
    case NETWORK_GETSOCKNAME:
      return "getsockname";
    case NETWORK_GETPEERNAME:
      return "getpeername";
    case NETWORK_SETSOCKOPT:
      return "setsockopt";
    case NETWORK_GETSOCKOPT:
      return "getsockopt";
    case NETWORK_SENDMSG:
      return "sendmsg";
    case NETWORK_RECVMSG:
      return "recvmsg";
    case NETWORK_READ:
      return "read";
    case NETWORK_WRITE:
      return "write";
    case NETWORK_READV:
      return "readv";
    case NETWORK_WRITEV:
      return "writev";
    case NETWORK_SENDMMSG:
      return "sendmmsg";
    case NETWORK_RECVMMSG:
      return "recvmmsg";
    default:
      return "unknown";
  }
}

const char* NetworkDirectionToString(uint32_t direction) {
  switch (direction) {
    case NETWORK_DIRECTION_INBOUND:
      return "inbound";
    case NETWORK_DIRECTION_OUTBOUND:
      return "outbound";
    default:
      return "unknown";
  }
}

const char* NetworkTransportToString(uint32_t transport) {
  switch (transport) {
    case NETWORK_TRANSPORT_TCP:
      return "tcp";
    case NETWORK_TRANSPORT_UDP:
      return "udp";
    case NETWORK_TRANSPORT_UNIX:
      return "unix";
    case NETWORK_TRANSPORT_RAW:
      return "raw";
    default:
      return "unknown";
  }
}

const char* ProcessEventKindToString(uint32_t kind) {
  switch (kind) {
    case PROCESS_EXEC:
      return "exec";
    case PROCESS_FORK:
    case PROCESS_CLONE:
    case PROCESS_CLONE3:
    case PROCESS_VFORK:
      return "start";
    case PROCESS_EXIT:
      return "exit";
    default:
      return "unknown";
  }
}

const char* FileEventKindToString(uint32_t kind) {
  switch (kind) {
    case FILE_OPENAT:
      return "open";
    case FILE_UNLINKAT:
      return "delete";
    case FILE_RENAMEAT2:
      return "rename";
    default:
      return "unknown";
  }
}

bool IsNetworkIoKind(uint32_t kind) {
  switch (kind) {
    case NETWORK_SENDTO:
    case NETWORK_RECVFROM:
    case NETWORK_SENDMSG:
    case NETWORK_RECVMSG:
    case NETWORK_READ:
    case NETWORK_WRITE:
    case NETWORK_READV:
    case NETWORK_WRITEV:
    case NETWORK_SENDMMSG:
    case NETWORK_RECVMMSG:
      return true;
    default:
      return false;
  }
}

/**
 *  Escape raw text for safe JSON string embedding.
 */
std::string JsonEscape(const char* s) {
  std::string out;
  if (!s) {
    return out;
  }

  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p != '\0'; ++p) {
    const unsigned char c = *p;
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[7] = {};
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }

  return out;
}

std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[7] = {};
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  return out;
}

/**
 *  Serialize common event header fields into JSON key/value pairs.
 */
void WriteHeader(std::ostream& out, const event_header& h) {
  out << "\"ts_ns\":" << h.ts_ns << ","
      << "\"type\":\"" << EventTypeToString(h.type) << "\","
      << "\"pid\":" << h.pid << ","
      << "\"tgid\":" << h.tgid << ","
      << "\"ppid\":" << h.ppid << ","
      << "\"uid\":" << h.uid << ","
      << "\"gid\":" << h.gid << ","
      << "\"comm\":\"" << JsonEscape(h.comm) << "\"";
}

void WriteProcessContext(std::ostream& out, const ProcessEntity* pe) {
  if (!pe || (pe->exec_path.empty() && pe->cmdline.empty() && pe->cwd.empty())) {
    return;
  }

  out << ",\"process_info\":{";
  bool has_field = false;
  const auto write_string = [&](const char* name, const std::string& value) {
    if (value.empty()) {
      return;
    }
    if (has_field) {
      out << ",";
    }
    out << "\"" << name << "\":\"" << JsonEscape(value) << "\"";
    has_field = true;
  };
  write_string("exec_path", pe->exec_path);
  write_string("cmdline", pe->cmdline);
  write_string("cwd", pe->cwd);
  write_string("parent_comm", pe->parent_comm);
  if (pe->start_time_ticks != 0) {
    if (has_field) {
      out << ",";
    }
    out << "\"start_time_ticks\":" << pe->start_time_ticks;
  }
  out << "}";
}

void WriteNetworkEndpoint(std::ostream& out, const char* label, const network_endpoint& ep) {
  const size_t addr_len = std::min<size_t>(ep.addr_len, sizeof(ep.addr));
  const std::string path =
      JsonEscape(std::string(ep.path, strnlen(ep.path, sizeof(ep.path))));
  const bool has_address = (ep.family == AF_INET && addr_len >= 4) ||
                           (ep.family == AF_INET6 && addr_len >= 16);
  if (ep.family == 0 && ep.port == 0 && !has_address && path.empty()) {
    return;
  }

  out << ",\"" << label << "\":{";
  bool has_field = false;
  if (ep.family != 0) {
    const char* family = ep.family == AF_INET     ? "ipv4"
                         : ep.family == AF_INET6  ? "ipv6"
                         : ep.family == AF_UNIX   ? "unix"
                                                   : "unknown";
    out << "\"family\":\"" << family << "\"";
    has_field = true;
  }
  if (ep.port != 0) {
    if (has_field) {
      out << ",";
    }
    out << "\"port\":" << ep.port;
    has_field = true;
  }
  if (has_address) {
    char address[INET6_ADDRSTRLEN] = {};
    if (inet_ntop(static_cast<int>(ep.family), ep.addr, address, sizeof(address))) {
      if (has_field) {
        out << ",";
      }
      out << "\"address\":\"" << address << "\"";
      has_field = true;
    }
  }
  if (!path.empty()) {
    if (has_field) {
      out << ",";
    }
    out << "\"path\":\"" << path << "\"";
  }
  out << "}";
}

void WriteEventBody(std::ostream& out, const CanonicalEvent& canonical) {
  const EventVariant& event = *canonical.raw;
  const ProcessEntity* pe = canonical.subject_process;

  std::visit(
      [&](const auto& ev) {
        WriteHeader(out, ev.hdr);
        using T = std::decay_t<decltype(ev)>;
        if constexpr (!std::is_same_v<T, process_event>) {
          WriteProcessContext(out, pe);
        }
        if (canonical.subject.id != 0) {
          out << ",\"entity_id\":" << canonical.subject.id;
        }

        if constexpr (std::is_same_v<T, process_event>) {
          out << ",\"event_name\":\"" << ProcessEventKindToString(ev.kind) << "\"";
          if (ev.kind == PROCESS_EXEC) {
            const std::string filename = pe ? pe->exec_path : ev.exec_path;
            if (!filename.empty()) {
              out << ",\"filename\":\"" << JsonEscape(filename) << "\"";
            }
            const std::string cmdline = pe ? pe->cmdline : ev.cmdline;
            if (!cmdline.empty()) {
              out << ",\"cmdline\":\"" << JsonEscape(cmdline) << "\"";
            }
            const std::string cwd = pe ? pe->cwd : ev.cwd;
            if (!cwd.empty()) {
              out << ",\"cwd\":\"" << JsonEscape(cwd) << "\"";
            }
          } else if (ev.kind == PROCESS_EXIT) {
            out << ",\"exit_code\":" << ev.exit_code;
          } else if (ev.child_pid != 0) {
            out << ",\"child_pid\":" << ev.child_pid;
          }
        } else if constexpr (std::is_same_v<T, file_event>) {
          out << ",\"event_name\":\"" << FileEventKindToString(ev.kind) << "\""
              << ",\"success\":" << (ev.ret >= 0 ? "true" : "false");
          if (ev.path_a[0] != '\0') {
            out << ",\"path\":\"" << JsonEscape(ev.path_a) << "\"";
          }
          if (ev.kind == FILE_RENAMEAT2 && ev.path_b[0] != '\0') {
            out << ",\"destination\":\"" << JsonEscape(ev.path_b) << "\"";
          }
          if (ev.ret < 0) {
            out << ",\"error_code\":" << -static_cast<int64_t>(ev.ret);
          }
        } else if constexpr (std::is_same_v<T, syscall_event>) {
          out << ",\"syscall_nr\":" << ev.syscall_nr
              << ",\"success\":" << (ev.ret >= 0 ? "true" : "false");
          if (ev.ret < 0) {
            out << ",\"error_code\":" << -ev.ret;
          }
        } else if constexpr (std::is_same_v<T, network_event>) {
          if (IsNetworkIoKind(ev.kind)) {
            if (ev.ret == 0 && ev.direction == NETWORK_DIRECTION_INBOUND) {
              out << ",\"event_name\":\"io_eof\""
                  << ",\"operation\":\"" << NetworkEventKindToString(ev.kind) << "\"";
            } else if (ev.ret < 0) {
              out << ",\"event_name\":\"io_error\""
                  << ",\"operation\":\"" << NetworkEventKindToString(ev.kind) << "\""
                  << ",\"error_code\":" << -static_cast<int64_t>(ev.ret);
            } else {
              out << ",\"event_name\":\"" << NetworkEventKindToString(ev.kind) << "\"";
            }
            out << ",\"fd\":" << ev.fd
                << ",\"direction\":\"" << NetworkDirectionToString(ev.direction) << "\""
                << ",\"transport\":\"" << NetworkTransportToString(ev.transport) << "\"";
          } else {
            out << ",\"event_name\":\"" << NetworkEventKindToString(ev.kind) << "\"";
            out << ",\"success\":" << (ev.ret >= 0 ? "true" : "false");
            if (ev.kind == NETWORK_SOCKET || ev.kind == NETWORK_SOCKETPAIR) {
              if (ev.ret >= 0) {
                out << ",\"fd\":" << ev.ret;
              }
              if (ev.kind == NETWORK_SOCKETPAIR && ev.peer_fd >= 0) {
                out << ",\"peer_fd\":" << ev.peer_fd;
              }
              out << ",\"domain\":" << ev.domain
                  << ",\"socket_type\":" << ev.sock_type
                  << ",\"protocol\":" << ev.protocol;
            } else {
              out << ",\"fd\":" << ev.fd;
              if ((ev.kind == NETWORK_ACCEPT || ev.kind == NETWORK_ACCEPT4) && ev.ret >= 0) {
                out << ",\"accepted_fd\":" << ev.ret;
              }
              if (ev.kind == NETWORK_LISTEN) {
                out << ",\"backlog\":" << ev.backlog;
              } else if (ev.kind == NETWORK_SHUTDOWN) {
                out << ",\"how\":" << ev.how;
              }
              if (ev.kind == NETWORK_CONNECT || ev.kind == NETWORK_ACCEPT ||
                  ev.kind == NETWORK_ACCEPT4) {
                WriteNetworkEndpoint(out, "remote", ev.remote);
              } else if (ev.kind == NETWORK_BIND) {
                WriteNetworkEndpoint(out, "local", ev.local);
              }
            }
            if (ev.transport != NETWORK_TRANSPORT_UNKNOWN) {
              out << ",\"transport\":\"" << NetworkTransportToString(ev.transport) << "\"";
            }
            if (ev.direction != NETWORK_DIRECTION_UNKNOWN) {
              out << ",\"direction\":\"" << NetworkDirectionToString(ev.direction) << "\"";
            }
            if (ev.ret < 0) {
              out << ",\"error_code\":" << -static_cast<int64_t>(ev.ret);
            }
          }
        }
      },
      event);
}

}  // namespace

bool JsonSink::NetworkSummaryKey::operator<(const NetworkSummaryKey& other) const {
  return std::tie(tgid, fd, direction, transport, local_family, local_port, local_address,
                  local_path, remote_family, remote_port, remote_address, remote_path) <
         std::tie(other.tgid, other.fd, other.direction, other.transport, other.local_family,
                  other.local_port, other.local_address, other.local_path, other.remote_family,
                  other.remote_port, other.remote_address, other.remote_path);
}

JsonSink::JsonSink(std::string file_path, uint64_t max_file_size_bytes)
    : file_path_(std::move(file_path)), max_file_size_bytes_(max_file_size_bytes) {
  last_flush_mono_ns_ = NowMonoNs();
  ready_ = OpenFileAppend();
}

JsonSink::~JsonSink() { FlushPending(); }

bool JsonSink::OpenFileAppend() {
  std::error_code ec;

  const std::filesystem::path p(file_path_);
  const auto parent = p.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, ec);
    ec.clear();
  }

  current_file_size_bytes_ = 0;
  if (std::filesystem::exists(p, ec)) {
    current_file_size_bytes_ = std::filesystem::file_size(p, ec);
    if (ec) {
      current_file_size_bytes_ = 0;
      ec.clear();
    }
  }

  file_out_.open(file_path_, std::ios::out | std::ios::app);
  return file_out_.is_open();
}

bool JsonSink::RotateIfNeeded(size_t next_record_bytes) {
  if (max_file_size_bytes_ == 0) {
    return true;
  }

  const uint64_t next_size = current_file_size_bytes_ + static_cast<uint64_t>(next_record_bytes);
  if (next_size <= max_file_size_bytes_) {
    return true;
  }

  ForceFlush();
  file_out_.close();

  std::error_code ec;
  std::filesystem::remove(file_path_, ec);
  ec.clear();

  current_file_size_bytes_ = 0;
  file_out_.open(file_path_, std::ios::out | std::ios::trunc);
  if (!file_out_.is_open()) {
    return false;
  }
  file_out_.close();

  file_out_.open(file_path_, std::ios::out | std::ios::app);
  return file_out_.is_open();
}

void JsonSink::MaybeFlush() {
  if (!file_out_.is_open()) {
    return;
  }

  static constexpr uint64_t kFlushEveryRecords = 256;
  static constexpr uint64_t kFlushIntervalNs = 250000000ULL;

  const uint64_t now_ns = NowMonoNs();
  if (pending_records_since_flush_ < kFlushEveryRecords &&
      (now_ns - last_flush_mono_ns_) < kFlushIntervalNs) {
    return;
  }

  file_out_.flush();
  pending_records_since_flush_ = 0;
  last_flush_mono_ns_ = now_ns;
}

void JsonSink::ForceFlush() {
  if (!file_out_.is_open()) {
    return;
  }

  file_out_.flush();
  pending_records_since_flush_ = 0;
  last_flush_mono_ns_ = NowMonoNs();
}

void JsonSink::WriteSerialized(const std::string& serialized) {
  if (!ready_ || !RotateIfNeeded(serialized.size())) {
    ready_ = false;
    return;
  }
  file_out_ << serialized;
  current_file_size_bytes_ += static_cast<uint64_t>(serialized.size());
  ++pending_records_since_flush_;
  MaybeFlush();
}

void JsonSink::AccumulateNetworkIo(const CanonicalEvent& canonical, const network_event& raw) {
  const auto address_bytes = [](const network_endpoint& endpoint) {
    const size_t size = std::min<size_t>(endpoint.addr_len, sizeof(endpoint.addr));
    return std::string(reinterpret_cast<const char*>(endpoint.addr), size);
  };
  const auto endpoint_path = [](const network_endpoint& endpoint) {
    return std::string(endpoint.path, strnlen(endpoint.path, sizeof(endpoint.path)));
  };

  NetworkSummaryKey key;
  key.tgid = raw.hdr.tgid;
  key.fd = raw.fd;
  key.direction = raw.direction;
  key.transport = raw.transport;
  key.local_family = raw.local.family;
  key.local_port = raw.local.port;
  key.local_address = address_bytes(raw.local);
  key.local_path = endpoint_path(raw.local);
  key.remote_family = raw.remote.family;
  key.remote_port = raw.remote.port;
  key.remote_address = address_bytes(raw.remote);
  key.remote_path = endpoint_path(raw.remote);

  auto [it, inserted] = network_io_summaries_.try_emplace(key);
  NetworkIoSummary& summary = it->second;
  if (inserted) {
    summary.header = raw.hdr;
    summary.entity_id = canonical.subject.id;
    summary.first_ts_ns = raw.hdr.ts_ns;
    summary.direction = raw.direction;
    summary.transport = raw.transport;
    summary.fd = raw.fd;
    summary.local = raw.local;
    summary.remote = raw.remote;
    if (canonical.subject_process) {
      summary.exec_path = canonical.subject_process->exec_path;
      summary.cmdline = canonical.subject_process->cmdline;
    }
  }

  summary.last_ts_ns = raw.hdr.ts_ns;
  summary.last_update_mono_ns = NowMonoNs();
  if (summary.operations != std::numeric_limits<uint64_t>::max()) {
    ++summary.operations;
  }
  if (raw.bytes_transferred > std::numeric_limits<uint64_t>::max() - summary.bytes_transferred) {
    summary.bytes_transferred = std::numeric_limits<uint64_t>::max();
  } else {
    summary.bytes_transferred += raw.bytes_transferred;
  }
}

void JsonSink::RememberNetworkContext(const network_event& raw) {
  const auto update = [&](int32_t fd, bool update_local, bool update_remote) {
    if (fd < 0) {
      return;
    }
    auto& context = network_socket_contexts_[{raw.hdr.tgid, fd}];
    if (raw.transport != NETWORK_TRANSPORT_UNKNOWN) {
      context.transport = raw.transport;
    }
    if (update_local && raw.local.family != 0) {
      context.local = raw.local;
    }
    if (update_remote && raw.remote.family != 0) {
      context.remote = raw.remote;
    }
  };

  switch (raw.kind) {
    case NETWORK_SOCKET:
      if (raw.ret >= 0) {
        network_socket_contexts_[{raw.hdr.tgid, raw.ret}] = {};
        update(raw.ret, true, true);
      }
      break;
    case NETWORK_SOCKETPAIR:
      if (raw.ret == 0) {
        network_socket_contexts_[{raw.hdr.tgid, raw.fd}] = {};
        network_socket_contexts_[{raw.hdr.tgid, raw.peer_fd}] = {};
        update(raw.fd, true, true);
        update(raw.peer_fd, true, true);
      }
      break;
    case NETWORK_CONNECT:
      if (raw.ret == 0) {
        update(raw.fd, false, true);
      }
      break;
    case NETWORK_BIND:
      if (raw.ret == 0) {
        update(raw.fd, true, false);
      }
      break;
    case NETWORK_ACCEPT:
    case NETWORK_ACCEPT4:
      if (raw.ret >= 0) {
        network_socket_contexts_[{raw.hdr.tgid, raw.ret}] = {};
        update(raw.ret, false, true);
      }
      break;
    case NETWORK_CLOSE:
      network_socket_contexts_.erase({raw.hdr.tgid, raw.fd});
      break;
    default:
      break;
  }

  static constexpr size_t kMaxSocketContexts = 16384;
  if (network_socket_contexts_.size() > kMaxSocketContexts) {
    network_socket_contexts_.erase(network_socket_contexts_.begin());
  }
}

void JsonSink::WriteNetworkSummary(const NetworkIoSummary& summary) {
  event_header header = summary.header;
  header.ts_ns = summary.last_ts_ns;

  std::ostringstream line;
  line << "{";
  WriteHeader(line, header);
  line << ",\"event_name\":\"io_summary\""
       << ",\"interval_start_ns\":" << summary.first_ts_ns
       << ",\"interval_end_ns\":" << summary.last_ts_ns
       << ",\"fd\":" << summary.fd
       << ",\"direction\":\"" << NetworkDirectionToString(summary.direction) << "\""
       << ",\"transport\":\"" << NetworkTransportToString(summary.transport) << "\""
       << ",\"operations\":" << summary.operations
       << ",\"bytes_transferred\":" << summary.bytes_transferred;
  if (summary.entity_id != 0) {
    line << ",\"entity_id\":" << summary.entity_id;
  }
  if (!summary.exec_path.empty() || !summary.cmdline.empty()) {
    line << ",\"process_info\":{";
    bool has_field = false;
    if (!summary.exec_path.empty()) {
      line << "\"exec_path\":\"" << JsonEscape(summary.exec_path) << "\"";
      has_field = true;
    }
    if (!summary.cmdline.empty()) {
      if (has_field) {
        line << ",";
      }
      line << "\"cmdline\":\"" << JsonEscape(summary.cmdline) << "\"";
    }
    line << "}";
  }
  WriteNetworkEndpoint(line, "local", summary.local);
  WriteNetworkEndpoint(line, "remote", summary.remote);
  line << "}\n";
  WriteSerialized(line.str());
}

void JsonSink::FlushDue() {
  static constexpr uint64_t kNetworkSummaryIntervalNs = 5000000000ULL;
  const uint64_t now_ns = NowMonoNs();
  for (auto it = network_io_summaries_.begin(); it != network_io_summaries_.end();) {
    if (now_ns - it->second.last_update_mono_ns < kNetworkSummaryIntervalNs) {
      ++it;
      continue;
    }
    WriteNetworkSummary(it->second);
    it = network_io_summaries_.erase(it);
  }
}

void JsonSink::FlushPending() {
  for (const auto& [key, summary] : network_io_summaries_) {
    (void)key;
    WriteNetworkSummary(summary);
  }
  network_io_summaries_.clear();
  ForceFlush();
}

void JsonSink::Write(const CanonicalEvent& event) {
  if (!ready_) {
    return;
  }
  if (!event.raw) {
    return;
  }

  FlushDue();
  if (const auto* raw = std::get_if<network_event>(event.raw)) {
    RememberNetworkContext(*raw);
    if (IsNetworkIoKind(raw->kind)) {
      if (raw->ret > 0 && raw->bytes_transferred > 0) {
        network_event enriched = *raw;
        const auto context = network_socket_contexts_.find({raw->hdr.tgid, raw->fd});
        if (context != network_socket_contexts_.end()) {
          if (enriched.transport == NETWORK_TRANSPORT_UNKNOWN) {
            enriched.transport = context->second.transport;
          }
          if (enriched.local.family == 0) {
            enriched.local = context->second.local;
          }
          if (enriched.remote.family == 0) {
            enriched.remote = context->second.remote;
          }
        }
        AccumulateNetworkIo(event, enriched);
        return;
      }
      if (raw->ret == 0 && raw->direction != NETWORK_DIRECTION_INBOUND) {
        return;
      }
    }
    switch (raw->kind) {
      case NETWORK_GETSOCKNAME:
      case NETWORK_GETPEERNAME:
      case NETWORK_GETSOCKOPT:
      case NETWORK_SETSOCKOPT:
        return;
      default:
        break;
    }
    if (IsNetworkIoKind(raw->kind) && (raw->ret == -EAGAIN || raw->ret == -EINTR)) {
      return;
    }
  }

  std::ostringstream line;
  line << "{";
  WriteEventBody(line, event);
  line << "}\n";
  WriteSerialized(line.str());
}


}  // namespace event_logger
