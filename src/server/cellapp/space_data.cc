#include "space_data.h"

namespace atlas {

auto SpaceData::Set(KeyId key, std::span<const uint8_t> value) -> bool {
  auto it = data_.find(key);
  if (it != data_.end()) {
    if (it->second.size() == value.size() &&
        std::equal(value.begin(), value.end(), it->second.begin())) {
      return false;
    }
    it->second.assign(value.begin(), value.end());
    return true;
  }
  data_.emplace(key, ValueBytes(value.begin(), value.end()));
  return true;
}

auto SpaceData::Remove(KeyId key) -> bool {
  return data_.erase(key) > 0;
}

auto SpaceData::Get(KeyId key) const -> const ValueBytes* {
  auto it = data_.find(key);
  return it == data_.end() ? nullptr : &it->second;
}

auto SpaceData::Contains(KeyId key) const -> bool {
  return data_.count(key) > 0;
}

auto SpaceData::Snapshot() const -> std::vector<Entry> {
  std::vector<Entry> out;
  out.reserve(data_.size());
  for (const auto& [k, v] : data_) out.emplace_back(k, v);
  return out;
}

}  // namespace atlas
