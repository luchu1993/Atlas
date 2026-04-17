#ifndef ATLAS_LIB_FOUNDATION_CONTAINERS_FLAT_MAP_H_
#define ATLAS_LIB_FOUNDATION_CONTAINERS_FLAT_MAP_H_

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

namespace atlas {

template <typename Key, typename Value, typename Compare = std::less<Key>>
class FlatMap {
 public:
  using value_type = std::pair<Key, Value>;
  using iterator = typename std::vector<value_type>::iterator;
  using const_iterator = typename std::vector<value_type>::const_iterator;

  auto Insert(const Key& key, const Value& value) -> std::pair<iterator, bool> {
    auto it = LowerBound(key);
    if (it != data_.end() && !comp_(key, it->first)) {
      return {it, false};
    }
    return {data_.insert(it, {key, value}), true};
  }

  auto InsertOrAssign(const Key& key, Value value) -> std::pair<iterator, bool> {
    auto it = LowerBound(key);
    if (it != data_.end() && !comp_(key, it->first)) {
      it->second = std::move(value);
      return {it, false};
    }
    return {data_.insert(it, {key, std::move(value)}), true};
  }

  auto Find(const Key& key) -> iterator {
    auto it = LowerBound(key);
    if (it != data_.end() && !comp_(key, it->first)) {
      return it;
    }
    return data_.end();
  }

  auto Find(const Key& key) const -> const_iterator {
    auto it = LowerBound(key);
    if (it != data_.end() && !comp_(key, it->first)) {
      return it;
    }
    return data_.end();
  }

  auto Erase(const Key& key) -> bool {
    auto it = Find(key);
    if (it == data_.end()) return false;
    data_.erase(it);
    return true;
  }

  auto Erase(iterator pos) -> iterator { return data_.erase(pos); }

  auto operator[](const Key& key) -> Value& {
    auto it = LowerBound(key);
    if (it != data_.end() && !comp_(key, it->first)) {
      return it->second;
    }
    it = data_.insert(it, {key, Value{}});
    return it->second;
  }

  [[nodiscard]] auto Contains(const Key& key) const -> bool { return Find(key) != data_.end(); }

  [[nodiscard]] auto size() const -> std::size_t { return data_.size(); }
  [[nodiscard]] auto empty() const -> bool { return data_.empty(); }
  void Clear() { data_.clear(); }
  void Reserve(std::size_t n) { data_.reserve(n); }

  auto begin() -> iterator { return data_.begin(); }
  auto end() -> iterator { return data_.end(); }
  auto begin() const -> const_iterator { return data_.begin(); }
  auto end() const -> const_iterator { return data_.end(); }

 private:
  auto LowerBound(const Key& key) -> iterator {
    return std::lower_bound(
        data_.begin(), data_.end(), key,
        [this](const value_type& elem, const Key& k) { return comp_(elem.first, k); });
  }

  auto LowerBound(const Key& key) const -> const_iterator {
    return std::lower_bound(
        data_.begin(), data_.end(), key,
        [this](const value_type& elem, const Key& k) { return comp_(elem.first, k); });
  }

  std::vector<value_type> data_;
  Compare comp_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_FOUNDATION_CONTAINERS_FLAT_MAP_H_
