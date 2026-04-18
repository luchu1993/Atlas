#ifndef ATLAS_SERVER_MACHINED_PROCESS_REGISTRY_H_
#define ATLAS_SERVER_MACHINED_PROCESS_REGISTRY_H_

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "network/address.h"
#include "network/channel.h"
#include "server/server_config.h"

namespace atlas::machined {

// ============================================================================
// ProcessEntry — one registered process
// ============================================================================

struct ProcessEntry {
  ProcessType process_type{ProcessType::kBaseApp};
  std::string name;
  Address internal_addr;
  Address external_addr;  // {0,0} if N/A
  uint32_t pid{0};
  float load{0.0f};

  // Backpointer to the live TCP channel.  Null if the process has disconnected
  // but cleanup has not yet run.  MUST be accessed only from the dispatcher thread.
  Channel* channel{nullptr};
};

// ============================================================================
// ProcessRegistry
// ============================================================================

class ProcessRegistry {
 public:
  // Returns false if the entry would violate uniqueness rules:
  //   - (process_type, name) pair must be unique.
  //   - channel must not already be associated with another entry.
  [[nodiscard]] auto RegisterProcess(ProcessEntry entry) -> bool;

  // Remove by channel pointer.  Returns the removed entry, or nullopt if not found.
  auto UnregisterByChannel(Channel* channel) -> std::optional<ProcessEntry>;

  // Remove by (type, name).  Returns the removed entry, or nullopt if not found.
  auto UnregisterByName(ProcessType type, const std::string& name) -> std::optional<ProcessEntry>;

  // Query — returns a snapshot (copies)
  [[nodiscard]] auto FindByType(ProcessType type) const -> std::vector<ProcessEntry>;
  [[nodiscard]] auto FindByName(ProcessType type, const std::string& name) const
      -> std::optional<ProcessEntry>;
  [[nodiscard]] auto FindByChannel(Channel* channel) const -> std::optional<ProcessEntry>;

  // Update load for a connected process
  void UpdateLoad(Channel* channel, float load, uint32_t entity_count);

  // Look up the TCP channel for a process by PID (+ optional source IP) to
  // correlate UDP heartbeat datagrams with their TCP registration channel
  // even when many processes share the same host IP.
  [[nodiscard]] auto FindTcpChannelByPid(uint32_t pid, uint32_t ip = 0) const -> Channel*;

  // Legacy fallback used only when older clients omit PID from the heartbeat.
  [[nodiscard]] auto FindTcpChannelByIp(uint32_t ip) const -> Channel*;

  [[nodiscard]] auto Size() const -> std::size_t { return entries_.size(); }

  // Iterate all entries (read-only)
  using VisitorFn = std::function<void(const ProcessEntry&)>;
  void ForEach(VisitorFn fn) const;

 private:
  std::vector<ProcessEntry> entries_;
};

}  // namespace atlas::machined

#endif  // ATLAS_SERVER_MACHINED_PROCESS_REGISTRY_H_
