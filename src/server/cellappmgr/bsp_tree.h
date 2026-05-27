#ifndef ATLAS_SERVER_CELLAPPMGR_BSP_TREE_H_
#define ATLAS_SERVER_CELLAPPMGR_BSP_TREE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "cellapp/cell_bounds.h"
#include "cellappmgr_messages.h"  // CellID
#include "foundation/error.h"
#include "network/address.h"
#include "serialization/binary_stream.h"

// BSP tree.
// CellAppMgr owns a BSPTree per Space. Leaves are Cells; internal nodes
// are axis-aligned split lines that recursively partition the Space on
// (x, z). At boot a fresh Space has a single leaf spanning +/-inf^2.
// Splits add cells for newly-joined CellApps; Balance() slides
// existing split lines to move load between siblings.
// Data-flow with CellApp:
//   CellAppMgr authoritative tree -> Serialize() -> UpdateGeometry blob ->
//     CellApp Deserialize() -> OffloadChecker / GhostMaintainer consume
//   CellApp InformCellLoad -> CellAppMgr sets leaf load/profile -> Balance()
// Serialisation intentionally omits balance state (aggression, prev
// direction) and runtime load/profile; those are CellAppMgr-local. Receivers
// never re-enter the balancer.

namespace atlas {

using CellLoadBuckets =
    std::array<uint32_t, cellappmgr::InformCellLoad::CellReport::kLoadBucketCount>;
using CellLoadCostBuckets =
    std::array<uint64_t, cellappmgr::InformCellLoad::CellReport::kLoadBucketCount>;

struct CellInfo {
  cellappmgr::CellID cell_id{0};
  Address cellapp_addr;
  CellBounds bounds;
  float load{0.f};           // authoritative only on CellAppMgr
  uint32_t entity_count{0};  // authoritative only on CellAppMgr
  float tick_load{0.f};
  uint64_t script_tick_us{0};
  uint64_t native_tick_us{0};
  uint32_t witness_count{0};
  uint32_t aoi_peer_count{0};
  uint64_t aoi_reliable_bytes{0};
  uint64_t aoi_unreliable_bytes{0};
  uint64_t backup_bytes{0};
  CellLoadBuckets x_buckets{};
  CellLoadBuckets z_buckets{};
  CellLoadCostBuckets x_load_buckets{};
  CellLoadCostBuckets z_load_buckets{};
};

enum class BSPAxis : uint8_t { kX = 0, kZ = 1 };

class BSPNode {
 public:
  virtual ~BSPNode() = default;

  // Point query. Returns the leaf covering (x, z), or nullptr if outside
  // the serving area (shouldn't happen once the root is properly rooted
  // at +/-inf bounds).
  [[nodiscard]] virtual auto FindCell(float x, float z) const -> const CellInfo* = 0;

  // Hysteresis-aware variant: returns the leaf whose primary region
  // (bounds shrunk by `ghost_region` on every split) covers (x, z).
  // Returns nullptr if the point lies inside any ancestor split's ghost
  // band, signalling the caller to defer migration until the entity
  // moves clearly past the boundary.
  [[nodiscard]] virtual auto FindPrimaryCell(float x, float z, float ghost_region) const
      -> const CellInfo* = 0;

  // Enumerate every leaf whose bounds overlap `rect`. Leaves are visited
  // in tree order (left-first, depth-first) - callers relying on
  // determinism can count on that.
  virtual void VisitRect(const CellBounds& rect,
                         const std::function<void(const CellInfo&)>& visitor) const = 0;

  // Propagate `sub_bounds` (the area this subtree is responsible for) down
  // to leaves, recomputing bounds along the way. Root is called with the
  // tree's root_bounds_; internal nodes split sub_bounds by axis/position_
  // for their children. Used after Balance() and after Split().
  virtual void PropagateBounds(const CellBounds& sub_bounds) = 0;

  // Bottom-up load aggregation. After this returns, every internal node's
  // {left_load_, right_load_} reflects the sum of its children's current
  // leaf loads. Leaves simply snapshot their own info.load.
  virtual void UpdateLoad() = 0;

  [[nodiscard]] virtual auto TotalLoad() const -> float = 0;

  // Move split lines in descendants. No-op on leaves. `safety_bound` guards
  // against pushing an already-overloaded side further into the red - if
  // the side that would grow is >= safety_bound, the internal holds still.
  virtual void Balance(float safety_bound) = 0;

  // Collect every leaf in tree order. Used by FindCellById and
  // serialization.
  virtual void CollectLeaves(std::vector<CellInfo*>& out) = 0;
  virtual void CollectLeaves(std::vector<const CellInfo*>& out) const = 0;

  // Pre-order serialization - internal nodes serialize themselves then
  // left, then right; leaves serialize themselves and stop. See Deserialize
  // free function.
  virtual void Serialize(BinaryWriter& w) const = 0;
};

class BSPLeaf : public BSPNode {
 public:
  explicit BSPLeaf(CellInfo info) : info_(std::move(info)) {}

  [[nodiscard]] auto Info() -> CellInfo& { return info_; }
  [[nodiscard]] auto Info() const -> const CellInfo& { return info_; }

  [[nodiscard]] auto FindCell(float x, float z) const -> const CellInfo* override;
  [[nodiscard]] auto FindPrimaryCell(float x, float z, float ghost_region) const
      -> const CellInfo* override;
  void VisitRect(const CellBounds& rect,
                 const std::function<void(const CellInfo&)>& visitor) const override;
  void PropagateBounds(const CellBounds& sub_bounds) override;
  void UpdateLoad() override;
  [[nodiscard]] auto TotalLoad() const -> float override { return info_.load; }
  void Balance(float /*safety_bound*/) override {}
  void CollectLeaves(std::vector<CellInfo*>& out) override { out.push_back(&info_); }
  void CollectLeaves(std::vector<const CellInfo*>& out) const override { out.push_back(&info_); }
  void Serialize(BinaryWriter& w) const override;

 private:
  CellInfo info_;
};

class BSPInternal : public BSPNode {
 public:
  BSPInternal(BSPAxis axis, float position, std::unique_ptr<BSPNode> left,
              std::unique_ptr<BSPNode> right)
      : axis_(axis), position_(position), left_(std::move(left)), right_(std::move(right)) {}

  [[nodiscard]] auto Axis() const -> BSPAxis { return axis_; }
  [[nodiscard]] auto Position() const -> float { return position_; }
  [[nodiscard]] auto Left() -> BSPNode* { return left_.get(); }
  [[nodiscard]] auto Right() -> BSPNode* { return right_.get(); }
  [[nodiscard]] auto Left() const -> const BSPNode* { return left_.get(); }
  [[nodiscard]] auto Right() const -> const BSPNode* { return right_.get(); }

  // Slot accessors used by BSPTree::Split to swap a descendant leaf for a
  // newly-created internal node in place. Intentionally mutable references.
  [[nodiscard]] auto LeftSlot() -> std::unique_ptr<BSPNode>& { return left_; }
  [[nodiscard]] auto RightSlot() -> std::unique_ptr<BSPNode>& { return right_; }

  // Unit-test hooks for the balance state machine.
  [[nodiscard]] auto AggressionForTest() const -> float { return aggression_; }
  [[nodiscard]] auto LeftLoadForTest() const -> float { return left_load_; }
  [[nodiscard]] auto RightLoadForTest() const -> float { return right_load_; }
  [[nodiscard]] auto CooldownForTest() const -> uint8_t { return balance_cooldown_ticks_; }

  [[nodiscard]] auto FindCell(float x, float z) const -> const CellInfo* override;
  [[nodiscard]] auto FindPrimaryCell(float x, float z, float ghost_region) const
      -> const CellInfo* override;
  void VisitRect(const CellBounds& rect,
                 const std::function<void(const CellInfo&)>& visitor) const override;
  void PropagateBounds(const CellBounds& sub_bounds) override;
  void UpdateLoad() override;
  [[nodiscard]] auto TotalLoad() const -> float override { return left_load_ + right_load_; }
  void Balance(float safety_bound) override;
  void CollectLeaves(std::vector<CellInfo*>& out) override {
    left_->CollectLeaves(out);
    right_->CollectLeaves(out);
  }
  void CollectLeaves(std::vector<const CellInfo*>& out) const override {
    left_->CollectLeaves(out);
    right_->CollectLeaves(out);
  }
  void Serialize(BinaryWriter& w) const override;

 private:
  enum class Direction : uint8_t { kNone, kLeft, kRight };

  BSPAxis axis_;
  float position_;
  std::unique_ptr<BSPNode> left_;
  std::unique_ptr<BSPNode> right_;

  // Balance state - NOT serialised. Fresh tree arrivals start at aggression
  // = 1.0 as if the balancer had never run.
  float left_load_{0.f};
  float right_load_{0.f};
  float aggression_{1.0f};
  Direction prev_direction_{Direction::kNone};
  uint8_t balance_cooldown_ticks_{0};
  // Our own sub-bounds, latched by PropagateBounds so Balance() can split
  // them for children after moving position_.
  CellBounds sub_bounds_;
};

class BSPTree {
 public:
  BSPTree() = default;

  // Initialise with a single leaf covering the whole space.
  void InitSingleCell(CellInfo info);

  // Split an existing leaf `cell_id`. `new_cell` gets the split's right/
  // positive side; the original leaf keeps the left/negative side, its
  // CellInfo (id + addr + load) untouched. `position` must lie strictly
  // inside the existing leaf's bounds on `axis` - otherwise the split is
  // degenerate and we return an error.
  auto Split(cellappmgr::CellID existing_cell_id, BSPAxis axis, float position, CellInfo new_cell)
      -> Result<void>;

  // Inverse of Split for the cellapp-death path. Removes the leaf with
  // `cell_id` and promotes its sibling subtree into the parent slot,
  // expanding the sibling's bounds to cover the merged region. Fails on
  // single-leaf trees (no sibling to promote).
  auto Unsplit(cellappmgr::CellID cell_id) -> Result<void>;

  [[nodiscard]] auto FindCell(float x, float z) const -> const CellInfo*;
  // Returns nullptr when the point sits within `ghost_region` metres of
  // any split — OffloadChecker uses this to defer migration across the
  // border buffer instead of probing ±h on every axis.
  [[nodiscard]] auto FindPrimaryCell(float x, float z, float ghost_region) const
      -> const CellInfo*;
  [[nodiscard]] auto FindCellById(cellappmgr::CellID id) const -> const CellInfo*;
  [[nodiscard]] auto FindCellByIdMutable(cellappmgr::CellID id) -> CellInfo*;

  // O(1) cached id of the originally-allocated leaf. Invariant: Split
  // preserves the original leaf on the left, so primary never changes.
  [[nodiscard]] auto PrimaryCellId() const -> cellappmgr::CellID { return primary_cell_id_; }
  void VisitRect(const CellBounds& rect, const std::function<void(const CellInfo&)>& visitor) const;

  void UpdateLoad();
  void Balance(float safety_bound);

  // Snapshot every leaf (handy for UpdateGeometry preparation, watchers).
  [[nodiscard]] auto Leaves() const -> std::vector<const CellInfo*>;
  [[nodiscard]] auto LeafSiblingPairs() const
      -> std::vector<std::pair<cellappmgr::CellID, cellappmgr::CellID>>;

  // Mutable leaf snapshot, for death rehoming: lets the caller rewrite
  // `cellapp_addr` + `load` on each leaf owned by a dead CellApp
  // without tearing down the tree. The returned pointers alias the
  // tree's owned storage and are invalidated by Split() / structural
  // edits; use them in a single synchronous pass.
  [[nodiscard]] auto LeavesMutable() -> std::vector<CellInfo*>;

  // Pre-order wire format. Balance state is intentionally excluded.
  void Serialize(BinaryWriter& w) const;
  static auto Deserialize(BinaryReader& r) -> Result<BSPTree>;

  [[nodiscard]] auto Empty() const -> bool { return root_ == nullptr; }
  [[nodiscard]] auto RootBounds() const -> const CellBounds& { return root_bounds_; }

  // Expose the root node for tests that want to assert on structure.
  [[nodiscard]] auto Root() const -> const BSPNode* { return root_.get(); }

 private:
  // Refreshed only on InitSingleCell / Deserialize; Split / Balance leave it
  // alone because the original leaf stays on the left side.
  void RecomputePrimaryCellId();

  std::unique_ptr<BSPNode> root_;
  CellBounds root_bounds_{};  // +/-inf by default
  cellappmgr::CellID primary_cell_id_{0};
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPPMGR_BSP_TREE_H_
