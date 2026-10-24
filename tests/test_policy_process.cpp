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

/** Baseline policy is a permissive pass-through today; lock that contract. */
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

}  // namespace

int main() {
  TestProcessEventAllowed();

  if (g_failures != 0) {
    std::printf("test_policy_process: %d failure(s)\n", g_failures);
    return 1;
  }

  std::printf("test_policy_process: OK\n");
  return 0;
}
