#include "lease_store.h"

#include <algorithm>
#include <chrono>

namespace atlas::machined {

namespace {

auto MsUntil(TimePoint t, TimePoint now) -> int64_t {
  if (t <= now) return 0;
  return std::chrono::duration_cast<std::chrono::milliseconds>(t - now).count();
}

}  // namespace

auto LeaseStore::Acquire(std::string_view key, std::string_view holder_id, uint32_t ttl_ms,
                         const Address& holder_addr, TimePoint now) -> AcquireOutcome {
  AcquireOutcome out;
  const auto expires_at =
      now + std::chrono::duration_cast<Duration>(std::chrono::milliseconds(ttl_ms));

  auto it = entries_.find(std::string(key));
  if (it == entries_.end()) {
    Entry entry;
    entry.holder_id = std::string(holder_id);
    entry.expires_at = expires_at;
    entry.holder_addr = holder_addr;
    entries_.emplace(std::string(key), std::move(entry));
    out.result = AcquireResult::kAcquired;
    return out;
  }

  Entry& existing = it->second;
  if (existing.holder_id == holder_id) {
    existing.expires_at = expires_at;
    existing.holder_addr = holder_addr;
    out.result = AcquireResult::kRenewed;
    return out;
  }

  if (existing.expires_at <= now) {
    existing.holder_id = std::string(holder_id);
    existing.expires_at = expires_at;
    existing.holder_addr = holder_addr;
    out.result = AcquireResult::kAcquired;
    return out;
  }

  out.result = AcquireResult::kRejected;
  out.current_holder = existing.holder_id;
  out.current_expires_in_ms = MsUntil(existing.expires_at, now);
  return out;
}

auto LeaseStore::Release(std::string_view key, std::string_view holder_id) -> bool {
  auto it = entries_.find(std::string(key));
  if (it == entries_.end()) return false;
  if (it->second.holder_id != holder_id) return false;
  entries_.erase(it);
  return true;
}

auto LeaseStore::PruneExpired(TimePoint now) -> std::size_t {
  std::size_t pruned = 0;
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (it->second.expires_at <= now) {
      it = entries_.erase(it);
      ++pruned;
    } else {
      ++it;
    }
  }
  return pruned;
}

auto LeaseStore::DropByHolderAddress(const Address& addr) -> std::size_t {
  std::size_t dropped = 0;
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (it->second.holder_addr == addr) {
      it = entries_.erase(it);
      ++dropped;
    } else {
      ++it;
    }
  }
  return dropped;
}

auto LeaseStore::Find(std::string_view key) const -> std::optional<Entry> {
  auto it = entries_.find(std::string(key));
  if (it == entries_.end()) return std::nullopt;
  return it->second;
}

}  // namespace atlas::machined
