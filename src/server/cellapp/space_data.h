#ifndef ATLAS_SERVER_CELLAPP_SPACE_DATA_H_
#define ATLAS_SERVER_CELLAPP_SPACE_DATA_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <utility>
#include <vector>

namespace atlas {

// Per-space key→bytes table. Ordered std::map keeps snapshot iteration
// deterministic so wire serialization and hot-reload diffs are stable.
class SpaceData {
 public:
  using KeyId = uint16_t;
  using ValueBytes = std::vector<uint8_t>;
  using Entry = std::pair<KeyId, ValueBytes>;

  SpaceData() = default;
  SpaceData(const SpaceData&) = delete;
  auto operator=(const SpaceData&) -> SpaceData& = delete;

  // True if the stored value changed (insert or overwrite with different
  // payload). Equal-value writes are no-ops so dirty broadcast skips them.
  auto Set(KeyId key, std::span<const uint8_t> value) -> bool;

  auto Remove(KeyId key) -> bool;

  [[nodiscard]] auto Get(KeyId key) const -> const ValueBytes*;
  [[nodiscard]] auto Contains(KeyId key) const -> bool;
  [[nodiscard]] auto Size() const -> std::size_t { return data_.size(); }
  [[nodiscard]] auto Empty() const -> bool { return data_.empty(); }

  // Ordered snapshot — used by new-witness init and new-cellapp handshake.
  [[nodiscard]] auto Snapshot() const -> std::vector<Entry>;

  template <typename Fn>
  void ForEach(Fn&& fn) const {
    for (const auto& [k, v] : data_) fn(k, v);
  }

  void Clear() { data_.clear(); }

 private:
  std::map<KeyId, ValueBytes> data_;
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_SPACE_DATA_H_
