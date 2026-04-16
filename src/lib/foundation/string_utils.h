#ifndef ATLAS_LIB_FOUNDATION_STRING_UTILS_H_
#define ATLAS_LIB_FOUNDATION_STRING_UTILS_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace atlas::string_utils {

// Splitting
[[nodiscard]] auto split(std::string_view str, char delimiter) -> std::vector<std::string_view>;
[[nodiscard]] auto split(std::string_view str, std::string_view delimiter)
    -> std::vector<std::string_view>;

// Trimming (whitespace)
[[nodiscard]] auto trim(std::string_view str) -> std::string_view;
[[nodiscard]] auto trim_left(std::string_view str) -> std::string_view;
[[nodiscard]] auto trim_right(std::string_view str) -> std::string_view;

// Case conversion (ASCII only)
[[nodiscard]] auto to_lower(std::string_view str) -> std::string;
[[nodiscard]] auto to_upper(std::string_view str) -> std::string;
[[nodiscard]] auto iequals(std::string_view a, std::string_view b) -> bool;

// Joining
[[nodiscard]] auto join(const std::vector<std::string_view>& parts, std::string_view separator)
    -> std::string;

// Replace
[[nodiscard]] auto replace_all(std::string_view str, std::string_view from, std::string_view to)
    -> std::string;

// Prefix/suffix
[[nodiscard]] constexpr auto starts_with(std::string_view str, std::string_view prefix) -> bool {
  return str.size() >= prefix.size() && str.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] constexpr auto ends_with(std::string_view str, std::string_view suffix) -> bool {
  return str.size() >= suffix.size() && str.substr(str.size() - suffix.size()) == suffix;
}

// Constexpr FNV-1a hash
[[nodiscard]] constexpr auto hash_fnv1a(std::string_view str) -> uint64_t {
  uint64_t hash = 14695981039346656037ULL;
  for (char c : str) {
    hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
    hash *= 1099511628211ULL;
  }
  return hash;
}

// String literal operator
namespace literals {
consteval auto operator""_hash(const char* str, std::size_t len) -> uint64_t {
  return hash_fnv1a(std::string_view{str, len});
}
}  // namespace literals

}  // namespace atlas::string_utils

#endif  // ATLAS_LIB_FOUNDATION_STRING_UTILS_H_
