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

struct ProcessEntry {
  ProcessType process_type{ProcessType::kBaseApp};
  std::string name;
  Address internal_addr;
  Address external_addr;
  uint32_t pid{0};
  float load{0.0f};

  // Dispatcher-thread backpointer to the live TCP registration channel.
  Channel* channel{nullptr};
};

class ProcessRegistry {
 public:
  // Rejects duplicate (process_type, name) pairs and reused channels.
  [[nodiscard]] auto RegisterProcess(ProcessEntry entry) -> bool;

  auto UnregisterByChannel(Channel* channel) -> std::optional<ProcessEntry>;

  auto UnregisterByName(ProcessType type, const std::string& name) -> std::optional<ProcessEntry>;

  [[nodiscard]] auto FindByType(ProcessType type) const -> std::vector<ProcessEntry>;
  [[nodiscard]] auto FindByName(ProcessType type, const std::string& name) const
      -> std::optional<ProcessEntry>;
  [[nodiscard]] auto FindByChannel(Channel* channel) const -> std::optional<ProcessEntry>;

  void UpdateLoad(Channel* channel, float load, uint32_t entity_count);

  // Look up the TCP channel for a process by PID (+ optional source IP) to
  // correlate UDP heartbeat datagrams with their TCP registration channel
  // even when many processes share the same host IP.
  [[nodiscard]] auto FindTcpChannelByPid(uint32_t pid, uint32_t ip = 0) const -> Channel*;

  // Legacy fallback used only when older clients omit PID from the heartbeat.
  [[nodiscard]] auto FindTcpChannelByIp(uint32_t ip) const -> Channel*;

  [[nodiscard]] auto Size() const -> std::size_t { return entries_.size(); }

  using VisitorFn = std::function<void(const ProcessEntry&)>;
  void ForEach(VisitorFn fn) const;

 private:
  std::vector<ProcessEntry> entries_;
};

}  // namespace atlas::machined

#endif  // ATLAS_SERVER_MACHINED_PROCESS_REGISTRY_H_
