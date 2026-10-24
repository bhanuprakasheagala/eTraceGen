#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <arpa/inet.h>
#include <unistd.h>

#include "sinks/json_sink.h"

namespace {

int g_failures = 0;

void Check(bool condition, const char* name) {
  if (condition) {
    std::printf("[pass] %s\n", name);
  } else {
    std::printf("[FAIL] %s\n", name);
    ++g_failures;
  }
}

void TestNetworkIoSummarized() {
  const std::string path =
      "/tmp/etracegen_json_sink_test_" + std::to_string(static_cast<long long>(getpid())) +
      ".ndjson";
  std::filesystem::remove(path);

  {
    event_logger::JsonSink sink(path, 0);
    Check(sink.IsReady(), "JSON sink opens test file");

    network_event connection{};
    connection.hdr.type = EVENT_TYPE_NETWORK;
    connection.hdr.pid = 123;
    connection.hdr.tgid = 123;
    connection.hdr.ts_ns = 90;
    connection.kind = NETWORK_CONNECT;
    connection.fd = 7;
    connection.ret = 0;
    connection.transport = NETWORK_TRANSPORT_TCP;
    connection.remote.family = AF_INET;
    connection.remote.port = 443;
    connection.remote.addr_len = 4;
    connection.remote.addr[0] = 203;
    connection.remote.addr[1] = 0;
    connection.remote.addr[2] = 113;
    connection.remote.addr[3] = 9;
    event_logger::EventVariant connection_variant = connection;
    event_logger::CanonicalEvent connection_canonical{};
    connection_canonical.raw = &connection_variant;
    sink.Write(connection_canonical);

    network_event raw{};
    raw.hdr.type = EVENT_TYPE_NETWORK;
    raw.hdr.pid = 123;
    raw.hdr.tgid = 123;
    raw.hdr.ts_ns = 100;
    raw.hdr.comm[0] = 'a';
    raw.kind = NETWORK_SENDMSG;
    raw.fd = 7;
    raw.ret = 12;
    raw.bytes_transferred = 12;
    raw.direction = NETWORK_DIRECTION_OUTBOUND;
    raw.transport = NETWORK_TRANSPORT_UNKNOWN;
    raw.local.family = AF_INET;
    raw.local.port = 1234;
    raw.local.addr_len = 4;
    raw.local.addr[0] = 127;
    raw.local.addr[3] = 1;

    event_logger::EventVariant event = raw;
    event_logger::CanonicalEvent canonical{};
    canonical.raw = &event;
    canonical.subject.id = 55;
    sink.Write(canonical);

    raw.hdr.ts_ns = 200;
    raw.ret = 18;
    raw.bytes_transferred = 18;
    event = raw;
    sink.Write(canonical);
    sink.FlushPending();
  }

  std::ifstream input(path);
  const std::string output((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
  Check(std::count(output.begin(), output.end(), '\n') == 2,
        "connection and I/O summary produce separate records");
  Check(output.find("\"event_name\":\"io_summary\"") != std::string::npos,
        "summary has an explicit event name");
  Check(output.find("\"operations\":2") != std::string::npos,
        "summary counts socket operations");
  Check(output.find("\"bytes_transferred\":30") != std::string::npos,
        "summary totals transferred bytes");
  Check(output.find("\"address\":\"203.0.113.9\"") != std::string::npos,
        "summary inherits its readable endpoint from the connection event");

  std::filesystem::remove(path);
}

void TestProcessAndFileFieldsAreRelevant() {
  const std::string path =
      "/tmp/etracegen_json_fields_test_" + std::to_string(static_cast<long long>(getpid())) +
      ".ndjson";
  std::filesystem::remove(path);

  {
    event_logger::JsonSink sink(path, 0);
    process_event process{};
    process.hdr.type = EVENT_TYPE_PROCESS;
    process.hdr.pid = 321;
    process.hdr.tgid = 321;
    process.kind = PROCESS_EXEC;
    std::snprintf(process.exec_path, sizeof(process.exec_path), "%s", "/usr/bin/demo");

    event_logger::EventVariant process_variant = process;
    event_logger::CanonicalEvent process_canonical{};
    process_canonical.raw = &process_variant;
    sink.Write(process_canonical);

    file_event file{};
    file.hdr.type = EVENT_TYPE_FILE;
    file.hdr.pid = 321;
    file.hdr.tgid = 321;
    file.kind = FILE_OPENAT;
    file.ret = 3;
    std::snprintf(file.path_a, sizeof(file.path_a), "%s", "/tmp/example");

    event_logger::EventVariant file_variant = file;
    event_logger::CanonicalEvent file_canonical{};
    file_canonical.raw = &file_variant;
    sink.Write(file_canonical);
    sink.FlushPending();
  }

  std::ifstream input(path);
  const std::string output((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
  Check(output.find("\"event_name\":\"exec\"") != std::string::npos &&
            output.find("\"filename\":\"/usr/bin/demo\"") != std::string::npos,
        "process record names exec and its executable");
  Check(output.find("\"exit_code\":0") == std::string::npos &&
            output.find("\"child_pid\":0") == std::string::npos,
        "process record omits irrelevant zero fields");
  Check(output.find("\"event_name\":\"open\"") != std::string::npos &&
            output.find("\"path\":\"/tmp/example\"") != std::string::npos &&
            output.find("\"success\":true") != std::string::npos,
        "file record names operation, path, and outcome");
  Check(output.find("\"path_b\":\"\"") == std::string::npos,
        "file record omits unused destination path");
  std::filesystem::remove(path);
}

}  // namespace

int main() {
  TestNetworkIoSummarized();
  TestProcessAndFileFieldsAreRelevant();
  if (g_failures != 0) {
    std::printf("test_json_sink: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("test_json_sink: OK\n");
  return 0;
}
