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

// In-memory leader-lease table. Clients (typically Reviver) ask machined
// to hold a named key on their behalf with a ttl. The lease expires if
// the holder doesn't renew before ttl elapses, at which point the next
// Acquire call can take it. This replaces the per-host file lock when
// the cluster spans multiple machines.
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

  // Acquire or renew. Returns kAcquired (first time or after expiry),
  // kRenewed (same holder refreshing), or kRejected (different holder
  // still inside ttl). holder_id may not be empty; ttl_ms must be > 0.
  auto Acquire(std::string_view key, std::string_view holder_id, uint32_t ttl_ms,
               const Address& holder_addr, TimePoint now) -> AcquireOutcome;

  // Returns true if released; false if no entry or holder_id mismatch.
  auto Release(std::string_view key, std::string_view holder_id) -> bool;

  // Drop all entries whose expires_at <= now. Returns number pruned.
  // Called from MachinedApp::OnTickComplete.
  auto PruneExpired(TimePoint now) -> std::size_t;

  // Drop every entry held by a specific Channel address (disconnect path).
  // Returns number pruned.
  auto DropByHolderAddress(const Address& addr) -> std::size_t;

  [[nodiscard]] auto Find(std::string_view key) const -> std::optional<Entry>;
  [[nodiscard]] auto size() const -> std::size_t { return entries_.size(); }

  // For tests / watchers.
  [[nodiscard]] auto Entries() const -> const std::unordered_map<std::string, Entry>& {
    return entries_;
  }

 private:
  std::unordered_map<std::string, Entry> entries_;
};

}  // namespace atlas::machined

#endif  // ATLAS_SERVER_MACHINED_LEASE_STORE_H_
