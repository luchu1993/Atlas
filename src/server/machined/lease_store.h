#ifndef ATLAS_SERVER_MACHINED_LEASE_STORE_H_
#define ATLAS_SERVER_MACHINED_LEASE_STORE_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "foundation/clock.h"
#include "network/address.h"

namespace atlas::machined {

// machined-served leader-lease registry; replaces the per-host file lock
// when a cluster spans multiple machines.
class LeaseStore {
 public:
  struct Entry {
    std::string holder_id;
    TimePoint expires_at{};
    Address holder_addr{};  // diagnostics only
  };

  enum class AcquireResult {
    kAcquired,
    kRenewed,
    kRejected,
  };

  struct AcquireOutcome {
    AcquireResult result{AcquireResult::kRejected};
    std::string current_holder;  // populated when kRejected
    int64_t current_expires_in_ms{0};
  };

  // holder_id must be non-empty; ttl_ms must be > 0.
  auto Acquire(std::string_view key, std::string_view holder_id, uint32_t ttl_ms,
               const Address& holder_addr, TimePoint now) -> AcquireOutcome;

  // Returns true if released; false if no entry or holder_id mismatch.
  auto Release(std::string_view key, std::string_view holder_id) -> bool;

  auto PruneExpired(TimePoint now) -> std::size_t;

  auto DropByHolderAddress(const Address& addr) -> std::size_t;

  [[nodiscard]] auto Find(std::string_view key) const -> std::optional<Entry>;
  [[nodiscard]] auto size() const -> std::size_t { return entries_.size(); }

  [[nodiscard]] auto PrunedTotal() const -> uint64_t { return pruned_total_; }
  [[nodiscard]] auto DroppedOnDisconnectTotal() const -> uint64_t {
    return dropped_on_disconnect_total_;
  }

  [[nodiscard]] auto Entries() const -> const std::unordered_map<std::string, Entry>& {
    return entries_;
  }

 private:
  std::unordered_map<std::string, Entry> entries_;
  uint64_t pruned_total_{0};
  uint64_t dropped_on_disconnect_total_{0};
};

}  // namespace atlas::machined

#endif  // ATLAS_SERVER_MACHINED_LEASE_STORE_H_
