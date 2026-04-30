#ifndef ATLAS_LIB_SERVER_ENTITY_TYPES_H_
#define ATLAS_LIB_SERVER_ENTITY_TYPES_H_

#include <cstdint>
#include <limits>
#include <random>

namespace atlas {

// ============================================================================
// EntityID — uniquely identifies a live entity within a server cluster
// ============================================================================

using EntityID = uint32_t;
inline constexpr EntityID kInvalidEntityID = 0;

// IDs in [1, kFirstLocalEntityID) are server-allocated by DBApp's
// EntityIdAllocator and cluster-stable. IDs in [kFirstLocalEntityID,
// 0xFFFFFFFF] are reserved for client-only entities (effects, HUD
// markers, prediction stand-ins) — the server must never mint into
// this range. Mirrors BigWorld's FIRST_LOCAL_ENTITY_ID convention.
inline constexpr EntityID kFirstLocalEntityID = 0x7F000000;

[[nodiscard]] inline constexpr auto IsServerEntityId(EntityID id) -> bool {
  return id != kInvalidEntityID && id < kFirstLocalEntityID;
}

// ============================================================================
// SpaceID — uniquely identifies a spatial partition. In the single-CellApp
// stage Space == Cell; the multi-CellApp path splits them.
// ============================================================================

using SpaceID = uint32_t;
inline constexpr SpaceID kInvalidSpaceID = 0;

// ============================================================================
// SessionKey — 32-byte random token issued to a client at login
// Used for authenticating the client <-> Proxy binding.
// ============================================================================

struct SessionKey {
  uint8_t bytes[32]{};

  [[nodiscard]] static auto Generate() -> SessionKey {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<uint64_t> dist;
    SessionKey key;
    for (int i = 0; i < 32; i += 8) {
      uint64_t v = dist(rng);
      for (int j = 0; j < 8; ++j) key.bytes[i + j] = static_cast<uint8_t>((v >> (j * 8)) & 0xFF);
    }
    return key;
  }

  [[nodiscard]] auto IsZero() const -> bool {
    for (auto b : bytes)
      if (b != 0) return false;
    return true;
  }

  bool operator==(const SessionKey& o) const {
    for (int i = 0; i < 32; ++i)
      if (bytes[i] != o.bytes[i]) return false;
    return true;
  }
  bool operator!=(const SessionKey& o) const { return !(*this == o); }
};

}  // namespace atlas

// Allow SessionKey to be used as a key in std::unordered_map
template <>
struct std::hash<atlas::SessionKey> {
  auto operator()(const atlas::SessionKey& k) const noexcept -> std::size_t {
    std::size_t h = 0;
    for (int i = 0; i < 32; i += 8) {
      uint64_t v = 0;
      for (int j = 0; j < 8; ++j) v |= static_cast<uint64_t>(k.bytes[i + j]) << (j * 8);
      h ^= std::hash<uint64_t>{}(v) + 0x9e3779b9 + (h << 6) + (h >> 2);
    }
    return h;
  }
};

#endif  // ATLAS_LIB_SERVER_ENTITY_TYPES_H_
