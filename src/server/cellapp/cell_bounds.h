#ifndef ATLAS_SERVER_CELLAPP_CELL_BOUNDS_H_
#define ATLAS_SERVER_CELLAPP_CELL_BOUNDS_H_

#include <algorithm>
#include <limits>

#include "serialization/binary_stream.h"

namespace atlas {

// ============================================================================
// CellBounds — axis-aligned 2D rectangle on (x, z) describing the portion of
// a Space this Cell owns.
//
// Phase 11 §3.6. Y is intentionally ignored: BSP partitioning runs on the
// horizontal plane (BigWorld convention). The bounds may be infinite on any
// edge — a fresh Space has exactly one Cell spanning [-inf, +inf]² until the
// first BSP split.
//
// Semantics:
//   - Half-open on max (contains returns true for points strictly < max_*)
//     so adjacent cells share an edge without double-ownership.
//   - Infinite edges use ±std::numeric_limits<float>::infinity(); callers
//     need never special-case them because std::isfinite/clamp treat them
//     uniformly.
// ============================================================================

struct CellBounds {
  float min_x{-std::numeric_limits<float>::infinity()};
  float min_z{-std::numeric_limits<float>::infinity()};
  float max_x{std::numeric_limits<float>::infinity()};
  float max_z{std::numeric_limits<float>::infinity()};

  [[nodiscard]] constexpr auto Contains(float x, float z) const -> bool {
    return x >= min_x && x < max_x && z >= min_z && z < max_z;
  }

  // Rectangle-rectangle intersection test (half-open both sides — two cells
  // that share an edge are NOT overlapping).
  [[nodiscard]] constexpr auto Overlaps(const CellBounds& o) const -> bool {
    return min_x < o.max_x && o.min_x < max_x && min_z < o.max_z && o.min_z < max_z;
  }

  // Area; returns +inf for unbounded cells — callers either filter or
  // accept the poison value.
  [[nodiscard]] auto Area() const -> float {
    const float w = std::max(0.f, max_x - min_x);
    const float h = std::max(0.f, max_z - min_z);
    return w * h;
  }

  // Wire format: four IEEE754 floats. Infinity survives the round-trip.
  void Serialize(BinaryWriter& w) const {
    w.Write(min_x);
    w.Write(min_z);
    w.Write(max_x);
    w.Write(max_z);
  }

  static auto Deserialize(BinaryReader& r) -> Result<CellBounds> {
    auto mx = r.Read<float>();
    auto mz = r.Read<float>();
    auto Mx = r.Read<float>();
    auto Mz = r.Read<float>();
    if (!mx || !mz || !Mx || !Mz)
      return Error{ErrorCode::kInvalidArgument, "CellBounds: truncated"};
    return CellBounds{*mx, *mz, *Mx, *Mz};
  }
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_CELL_BOUNDS_H_
