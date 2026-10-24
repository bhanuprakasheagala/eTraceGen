/*
 * File Notes:
 * - Suppresses noisy network metadata and routine transient I/O events.
 * - Keeps filtering decisions centralized before events reach the sink.
 */

#include "policy/policy.h"

#include <cerrno>

namespace event_logger {
namespace {

bool IsSocketIoKind(uint32_t kind) {
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

}  // namespace

/**
 * @brief Decide whether event is allowed to continue.
 *
 * @param event Typed event payload.
 * @return false for routine socket metadata lookups and expected transient I/O results.
 */
bool PolicyEngine::Allow(const EventVariant& event) const {
  const auto* network = std::get_if<network_event>(&event);
  if (!network) {
    return true;
  }

  switch (network->kind) {
    case NETWORK_GETSOCKNAME:
    case NETWORK_GETPEERNAME:
    case NETWORK_GETSOCKOPT:
    case NETWORK_SETSOCKOPT:
      return false;
    default:
      break;
  }

  if (!IsSocketIoKind(network->kind)) {
    return true;
  }

  if (network->ret == -EAGAIN || network->ret == -EINTR) {
    return false;
  }
  return network->ret != 0 || network->direction == NETWORK_DIRECTION_INBOUND;
}

}  // namespace event_logger
