/*
 * File Notes:
 * - Minimal unit test scaffold for the policy stage.
 * - Exists to keep the `test_policy_process` CMake target buildable and to grow
 *   into the registry/normalization tests introduced by the M1 knowledge layer.
 */

#include <cstdio>

#include "decoder/decoder.h"
#include "policy/policy.h"

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

/** Process events remain enabled in the default policy. */
void TestProcessEventAllowed() {
  event_logger::PolicyEngine policy;

  process_event ev{};
  ev.hdr.type = EVENT_TYPE_PROCESS;
  ev.hdr.pid = 1234;
  ev.hdr.tgid = 1234;
  ev.kind = PROCESS_EXEC;

  const event_logger::EventVariant event = ev;
  Check(policy.Allow(event), "process exec event is allowed by baseline policy");
}

void TestNetworkNoiseFiltered() {
  event_logger::PolicyEngine policy;

  network_event retry{};
  retry.kind = NETWORK_RECVMSG;
  retry.ret = -11;
  Check(!policy.Allow(event_logger::EventVariant{retry}),
        "routine nonblocking receive retry is suppressed");

  network_event option_lookup{};
  option_lookup.kind = NETWORK_GETSOCKOPT;
  option_lookup.ret = 0;
  Check(!policy.Allow(event_logger::EventVariant{option_lookup}),
        "socket option lookup is suppressed");
}

void TestNetworkIoAllowedForAggregation() {
  event_logger::PolicyEngine policy;

  network_event send{};
  send.kind = NETWORK_SENDMSG;
  send.ret = 128;
  Check(policy.Allow(event_logger::EventVariant{send}),
        "successful network I/O passes to summary aggregation");
}

}  // namespace

int main() {
  TestProcessEventAllowed();
  TestNetworkNoiseFiltered();
  TestNetworkIoAllowedForAggregation();

  if (g_failures != 0) {
    std::printf("test_policy_process: %d failure(s)\n", g_failures);
    return 1;
  }

  std::printf("test_policy_process: OK\n");
  return 0;
}
