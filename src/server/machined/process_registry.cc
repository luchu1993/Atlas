#include "machined/process_registry.h"

#include <algorithm>

#include "foundation/log.h"

namespace atlas::machined {

auto ProcessRegistry::RegisterProcess(ProcessEntry entry) -> bool {
  // Uniqueness: (process_type, name) must not already exist
  for (const auto& e : entries_) {
    if (e.process_type == entry.process_type && e.name == entry.name) {
      ATLAS_LOG_WARNING("ProcessRegistry: duplicate registration ({}, {})",
                        static_cast<int>(entry.process_type), entry.name);
      return false;
    }
    if (entry.channel != nullptr && e.channel == entry.channel) {
      ATLAS_LOG_WARNING("ProcessRegistry: channel already registered for another process");
      return false;
    }
  }

  ATLAS_LOG_INFO("ProcessRegistry: registered ({}, {}) pid={}",
                 static_cast<int>(entry.process_type), entry.name, entry.pid);
  entries_.push_back(std::move(entry));
  return true;
}

auto ProcessRegistry::UnregisterByChannel(Channel* channel) -> std::optional<ProcessEntry> {
  auto it = std::find_if(entries_.begin(), entries_.end(),
                         [channel](const ProcessEntry& e) { return e.channel == channel; });
  if (it == entries_.end()) return std::nullopt;

  ProcessEntry removed = std::move(*it);
  entries_.erase(it);
  ATLAS_LOG_INFO("ProcessRegistry: unregistered ({}, {}) via channel",
                 static_cast<int>(removed.process_type), removed.name);
  return removed;
}

auto ProcessRegistry::UnregisterByName(ProcessType type, const std::string& name)
    -> std::optional<ProcessEntry> {
  auto it = std::find_if(entries_.begin(), entries_.end(), [&](const ProcessEntry& e) {
    return e.process_type == type && e.name == name;
  });
  if (it == entries_.end()) return std::nullopt;

  ProcessEntry removed = std::move(*it);
  entries_.erase(it);
  ATLAS_LOG_INFO("ProcessRegistry: unregistered ({}, {}) by name",
                 static_cast<int>(removed.process_type), removed.name);
  return removed;
}

auto ProcessRegistry::FindByType(ProcessType type) const -> std::vector<ProcessEntry> {
  std::vector<ProcessEntry> result;
  for (const auto& e : entries_) {
    if (e.process_type == type) result.push_back(e);
  }
  return result;
}

auto ProcessRegistry::FindByName(ProcessType type, const std::string& name) const
    -> std::optional<ProcessEntry> {
  for (const auto& e : entries_) {
    if (e.process_type == type && e.name == name) return e;
  }
  return std::nullopt;
}

auto ProcessRegistry::FindByChannel(Channel* channel) const -> std::optional<ProcessEntry> {
  for (const auto& e : entries_) {
    if (e.channel == channel) return e;
  }
  return std::nullopt;
}

void ProcessRegistry::UpdateLoad(Channel* channel, float load, uint32_t /*entity_count*/) {
  for (auto& e : entries_) {
    if (e.channel == channel) {
      e.load = load;
      return;
    }
  }
}

void ProcessRegistry::ForEach(VisitorFn fn) const {
  for (const auto& e : entries_) fn(e);
}

auto ProcessRegistry::FindTcpChannelByIp(uint32_t ip) const -> Channel* {
  for (const auto& e : entries_) {
    if (e.channel != nullptr && e.internal_addr.Ip() == ip) return e.channel;
  }
  return nullptr;
}

auto ProcessRegistry::FindTcpChannelByPid(uint32_t pid, uint32_t ip) const -> Channel* {
  for (const auto& e : entries_) {
    if (e.channel == nullptr || e.pid != pid) {
      continue;
    }

    if (ip == 0 || e.internal_addr.Ip() == ip) {
      return e.channel;
    }
  }
  return nullptr;
}

}  // namespace atlas::machined
