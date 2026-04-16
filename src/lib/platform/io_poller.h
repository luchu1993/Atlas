#ifndef ATLAS_LIB_PLATFORM_IO_POLLER_H_
#define ATLAS_LIB_PLATFORM_IO_POLLER_H_

#include <cstdint>
#include <functional>
#include <memory>

#include "foundation/clock.h"
#include "foundation/error.h"

namespace atlas {

// ============================================================================
// Platform-specific file descriptor handle
// ============================================================================

#if ATLAS_PLATFORM_WINDOWS
using FdHandle = uintptr_t;  // SOCKET is UINT_PTR on Windows
#else
using FdHandle = int;
#endif

inline constexpr FdHandle kInvalidFd = static_cast<FdHandle>(-1);

// ============================================================================
// IOEvent flags
// ============================================================================

enum class IOEvent : uint8_t {
  kNone = 0x00,
  kReadable = 0x01,
  kWritable = 0x02,
  kError = 0x04,
  kHangUp = 0x08,
};

constexpr auto operator|(IOEvent a, IOEvent b) -> IOEvent {
  return static_cast<IOEvent>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

constexpr auto operator&(IOEvent a, IOEvent b) -> IOEvent {
  return static_cast<IOEvent>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

constexpr auto operator~(IOEvent a) -> IOEvent {
  return static_cast<IOEvent>(~static_cast<uint8_t>(a));
}

constexpr auto operator|=(IOEvent& a, IOEvent b) -> IOEvent& {
  a = a | b;
  return a;
}

constexpr auto operator&=(IOEvent& a, IOEvent b) -> IOEvent& {
  a = a & b;
  return a;
}

// ============================================================================
// IOCallback
// ============================================================================

using IOCallback = std::function<void(FdHandle fd, IOEvent events)>;

// ============================================================================
// IOPoller — abstract reactor-pattern IO multiplexer
// ============================================================================

class IOPoller {
 public:
  virtual ~IOPoller() = default;

  virtual auto Add(FdHandle fd, IOEvent interest, IOCallback callback) -> Result<void> = 0;
  virtual auto Modify(FdHandle fd, IOEvent interest) -> Result<void> = 0;
  virtual auto Remove(FdHandle fd) -> Result<void> = 0;
  virtual auto Poll(Duration max_wait) -> Result<int> = 0;

  [[nodiscard]] static auto Create() -> std::unique_ptr<IOPoller>;
};

}  // namespace atlas

#endif  // ATLAS_LIB_PLATFORM_IO_POLLER_H_
