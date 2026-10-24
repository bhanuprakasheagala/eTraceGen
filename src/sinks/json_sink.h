#ifndef EVENT_LOGGER_JSON_SINK_H
#define EVENT_LOGGER_JSON_SINK_H

/*
 * File Notes:
 * - Concrete sink that renders events as line-delimited JSON objects.
 * - File-only sink with max-size rollover.
 */

#include <cstdint>
#include <fstream>
#include <map>
#include <ostream>
#include <string>
#include <tuple>

#include "sinks/sink.h"

namespace event_logger {

/**
 * @brief Sink that writes one JSON object per event line into a file.
 */
class JsonSink final : public Sink {
 public:
  /**
   * @brief Construct file-backed JSON sink.
   *
   * File is opened in append mode. When size reaches limit, sink deletes the
   * file and starts a new one.
   *
   * @param file_path Destination NDJSON file path.
   * @param max_file_size_bytes Max bytes before rollover. 0 disables rollover.
   */
  JsonSink(std::string file_path, uint64_t max_file_size_bytes);

  /**
   * @brief Flush pending buffered data before sink teardown.
   */
  ~JsonSink() override;

  /**
   * @brief Return true when sink is ready to write events.
   */
  bool IsReady() const { return ready_; }

  /**
   * @brief Serialize and write one canonical event as JSON.
   */
  void Write(const CanonicalEvent& event) override;

  /**
   * @brief Emit pending socket-I/O summaries and flush the output file.
   */
  void FlushPending();

  /**
   * @brief Emit socket-I/O summaries whose aggregation interval has elapsed.
   */
  void FlushDue();

 private:
  struct NetworkSummaryKey {
    uint32_t tgid = 0;
    int32_t fd = 0;
    uint32_t direction = 0;
    uint32_t transport = 0;
    uint32_t local_family = 0;
    uint32_t local_port = 0;
    std::string local_address;
    std::string local_path;
    uint32_t remote_family = 0;
    uint32_t remote_port = 0;
    std::string remote_address;
    std::string remote_path;

    bool operator<(const NetworkSummaryKey& other) const;
  };

  struct NetworkIoSummary {
    event_header header{};
    uint64_t entity_id = 0;
    uint64_t first_ts_ns = 0;
    uint64_t last_ts_ns = 0;
    uint64_t last_update_mono_ns = 0;
    uint64_t operations = 0;
    uint64_t bytes_transferred = 0;
    uint32_t direction = NETWORK_DIRECTION_UNKNOWN;
    uint32_t transport = NETWORK_TRANSPORT_UNKNOWN;
    int32_t fd = 0;
    network_endpoint local{};
    network_endpoint remote{};
    std::string exec_path;
    std::string cmdline;
  };

  struct NetworkSocketContext {
    uint32_t transport = NETWORK_TRANSPORT_UNKNOWN;
    network_endpoint local{};
    network_endpoint remote{};
  };

  /**
   *  Open sink file in append mode and initialize file size state.
   */
  bool OpenFileAppend();
  /**
   *  Rotate sink file when appending next record would exceed max size.
   */
  bool RotateIfNeeded(size_t next_record_bytes);
  /**
   *  Periodic flush gate balancing durability and syscall overhead.
   */
  void MaybeFlush();
  /**
   *  Force immediate flush; used during rotation and teardown.
   */
  void ForceFlush();
  void AccumulateNetworkIo(const CanonicalEvent& event, const network_event& raw);
  void RememberNetworkContext(const network_event& raw);
  void WriteNetworkSummary(const NetworkIoSummary& summary);
  void WriteSerialized(const std::string& serialized);

  std::ofstream file_out_;
  std::string file_path_;
  uint64_t max_file_size_bytes_ = 0;
  uint64_t current_file_size_bytes_ = 0;

  uint64_t pending_records_since_flush_ = 0;
  uint64_t last_flush_mono_ns_ = 0;

  std::map<NetworkSummaryKey, NetworkIoSummary> network_io_summaries_;
  std::map<std::pair<uint32_t, int32_t>, NetworkSocketContext> network_socket_contexts_;

  bool ready_ = false;
};

}  // namespace event_logger

#endif
