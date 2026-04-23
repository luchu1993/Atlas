#ifndef ATLAS_SERVER_CELLAPPMGR_CELLAPPMGR_H_
#define ATLAS_SERVER_CELLAPPMGR_CELLAPPMGR_H_

#include <cstdint>
#include <unordered_map>

#include "bsp_tree.h"
#include "cellappmgr_messages.h"
#include "foundation/clock.h"
#include "server/entity_types.h"
#include "server/manager_app.h"

namespace atlas {

class Channel;

// CellAppMgr — CellApp cluster manager.
//
// Responsibilities:
//   • Accept CellApp registration, assign a cluster-wide app_id in
//     [1, 255]. CellApp derives its EntityID high byte from app_id so
//     IDs never collide across CellApps.
//   • Track CellApp load (InformCellLoad) and feed it into each Space's
//     BSPTree for per-cycle Balance().
//   • Answer CreateSpaceRequest by picking a hosting CellApp (least-
//     loaded), installing a single-cell partition, and pushing the
//     geometry out.
//   • Tick: every ~1 s, rebalance every Space's BSP tree and broadcast
//     the updated geometry. Broadcast is unconditional — the packets
//     are small and CellApp consumes idempotently. A change-detecting
//     send is a later optimisation.
//   • Detect CellApp death via machined and drop the peer from
//     cellapps_.

class CellAppMgr : public ManagerApp {
 public:
  static auto Run(int argc, char* argv[]) -> int;

  CellAppMgr(EventDispatcher& dispatcher, NetworkInterface& network);

  // ---- Exposed handlers (public so unit tests can poke them directly) ----
  void OnRegisterCellApp(const Address& src, Channel* ch, const cellappmgr::RegisterCellApp& msg);
  void OnInformCellLoad(const Address& src, Channel* ch, const cellappmgr::InformCellLoad& msg);
  void OnCreateSpaceRequest(const Address& src, Channel* ch,
                            const cellappmgr::CreateSpaceRequest& msg);

  // Called from machined Death notification; public so unit tests can
  // simulate a peer dying without bringing up a real machined.
  void OnCellAppDeath(const Address& internal_addr);

  // ---- Per-Space view for tests and watchers ----
  struct CellAppInfo {
    Address internal_addr;
    uint32_t app_id{0};
    float load{0.f};
    uint32_t entity_count{0};
    Channel* channel{nullptr};
    TimePoint registered_at{};
    TimePoint last_load_report_at{};
  };

  struct SpacePartition {
    SpaceID space_id{kInvalidSpaceID};
    BSPTree bsp;
    // Serialised tree bytes as of the last fan-out. BroadcastGeometry
    // compares against this and skips the send when the bytes match,
    // so a steady-state cluster stops re-broadcasting a tree nothing
    // asked to change.
    std::vector<std::byte> last_broadcast_blob;
  };

  [[nodiscard]] auto CellApps() const -> const std::unordered_map<Address, CellAppInfo>& {
    return cellapps_;
  }

  // Test-only: direct access to the BaseApp channel map so integration-
  // shaped unit tests can seed a synthetic BaseApp without going
  // through machined Birth. Production writers are the Subscribe
  // callbacks in Init().
  [[nodiscard]] auto BaseAppChannelsForTest() -> std::unordered_map<Address, Channel*>& {
    return baseapps_;
  }
  [[nodiscard]] auto Spaces() const -> const std::unordered_map<SpaceID, SpacePartition>& {
    return spaces_;
  }
  // Test hook — scenarios that need to seed a pre-split BSP (multi-cell
  // space) before running the balance pump reach in through this map.
  // Production writers are OnCreateSpaceRequest and OnCellAppDeath.
  [[nodiscard]] auto SpacesForTest() -> std::unordered_map<SpaceID, SpacePartition>& {
    return spaces_;
  }

  // Rebalance every Space and broadcast fresh UpdateGeometry. Public for
  // deterministic test invocation (production calls it from a 1 Hz
  // timer).
  void TickLoadBalance();

 protected:
  [[nodiscard]] auto Init(int argc, char* argv[]) -> bool override;
  void Fini() override;
  void RegisterWatchers() override;
  void OnTickComplete() override;

 private:
  // ---- Internal helpers ----
  [[nodiscard]] auto PickHostForNewSpace() const -> const CellAppInfo*;

  // Alternate-host picker used on CellApp death: returns the least-
  // loaded surviving CellApp that is NOT on `exclude_addr`'s IP when
  // possible, falling back to any survivor when all remaining
  // CellApps share that IP. Nullptr when no survivor exists. Two-tier
  // "prefer different machine, break ties on load" preference.
  [[nodiscard]] auto PickAlternateHost(const Address& exclude_addr) const -> const CellAppInfo*;

  void SendAddCell(const CellAppInfo& target, SpaceID space_id, cellappmgr::CellID cell_id,
                   const CellBounds& bounds);
  void BroadcastGeometry(SpacePartition& partition);

  // ---- State ----
  std::unordered_map<Address, CellAppInfo> cellapps_;
  std::unordered_map<SpaceID, SpacePartition> spaces_;

  // BaseApp channels, populated via machined Birth/Death. Used for the
  // CellAppDeath broadcast in OnCellAppDeath — every BaseApp gets told
  // which Reals it needs to restore to which new host.
  std::unordered_map<Address, Channel*> baseapps_;

  // EntityID cluster assignment: high 8 bits = app_id. We hand out
  // app_ids [1..255]; app_id 0 is reserved (matches kInvalidEntityID
  // prefix and keeps "uninitialised" distinguishable).
  uint32_t next_cellapp_app_id_{1};

  // Cell id pool (per Space): assigned 1-based on AddCellToSpace. Every
  // Space currently starts with exactly one Cell (id 1); splits come
  // later. Kept cluster-wide as a monotonic counter for simplicity.
  cellappmgr::CellID next_cell_id_{1};

  // Tick counter for the 1 Hz rebalance cadence. Running off OnTickComplete
  // keeps us consistent with the rest of the server loop; no separate
  // timer thread.
  uint64_t last_balance_tick_{0};

  static constexpr float kBalanceSafetyBound = 0.9f;
  static constexpr uint64_t kBalanceTickInterval = 30;  // ~1 s @ 30 Hz
  static constexpr uint32_t kMaxCellAppAppId = 255;
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPPMGR_CELLAPPMGR_H_
