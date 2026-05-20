#ifndef ATLAS_SERVER_CELLAPPMGR_CELLAPPMGR_H_
#define ATLAS_SERVER_CELLAPPMGR_CELLAPPMGR_H_

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "bsp_tree.h"
#include "cellappmgr_messages.h"
#include "foundation/clock.h"
#include "server/entity_types.h"
#include "server/manager_app.h"

namespace atlas {

class Channel;

class CellAppMgr : public ManagerApp {
 public:
  static auto Run(int argc, char* argv[]) -> int;

  CellAppMgr(EventDispatcher& dispatcher, NetworkInterface& network);

  void OnRegisterCellApp(const Address& src, Channel* ch, const cellappmgr::RegisterCellApp& msg);
  void OnInformCellLoad(const Address& src, Channel* ch, const cellappmgr::InformCellLoad& msg);
  void OnCreateSpaceRequest(const Address& src, Channel* ch,
                            const cellappmgr::CreateSpaceRequest& msg);

  void OnCellAppDeath(const Address& internal_addr);

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
    // Last fan-out bytes; stable trees skip redundant broadcasts.
    std::vector<std::byte> last_broadcast_blob;
    // Entity type name the primary host auto-spawns on AddCellToSpace.
    std::string space_master_type;
  };

  [[nodiscard]] auto CellApps() const -> const std::unordered_map<Address, CellAppInfo>& {
    return cellapps_;
  }

  [[nodiscard]] auto BaseAppChannelsForTest() -> std::unordered_map<Address, Channel*>& {
    return baseapps_;
  }
  [[nodiscard]] auto Spaces() const -> const std::unordered_map<SpaceID, SpacePartition>& {
    return spaces_;
  }
  [[nodiscard]] auto SpacesForTest() -> std::unordered_map<SpaceID, SpacePartition>& {
    return spaces_;
  }

  void TickLoadBalance();

 protected:
  [[nodiscard]] auto Init(int argc, char* argv[]) -> bool override;
  void Fini() override;
  void RegisterWatchers() override;
  void OnTickComplete() override;

 private:
  [[nodiscard]] auto PickHostForNewSpace() const -> const CellAppInfo*;

  // Sorted ascending by (load, app_id) for deterministic multi-cell
  // bootstrap; size capped at cellapps_.size().
  [[nodiscard]] auto SortedHostsForBootstrap(std::size_t max) const
      -> std::vector<const CellAppInfo*>;

  // BFS N-1 splits with alternating X/Z axes; N=4 lands as a 2x2 grid.
  // Each new leaf takes hosts[i] in order; partial bootstrap on Split error.
  void BootstrapMultiCellPartition(SpacePartition& partition,
                                   const std::vector<const CellAppInfo*>& hosts);

  [[nodiscard]] auto PickAlternateHost(const Address& exclude_addr) const -> const CellAppInfo*;

  // Prefers a survivor that already holds a leaf of this space — its
  // ghost SpaceData replica is up to date, ideal for owner handoff.
  [[nodiscard]] auto PickAlternateHostInSpace(const Address& exclude_addr,
                                              const SpacePartition& partition) const
      -> const CellAppInfo*;

  void SendAddCell(const CellAppInfo& target, SpaceID space_id, cellappmgr::CellID cell_id,
                   const CellBounds& bounds, bool is_primary,
                   const std::string& space_master_type);
  void BroadcastGeometry(SpacePartition& partition);

  std::unordered_map<Address, CellAppInfo> cellapps_;
  std::unordered_map<SpaceID, SpacePartition> spaces_;
  // CreateSpaceRequests received before any CellApp registered; drained the
  // moment the first CellApp registers. Each entry is (msg, src, channel).
  struct PendingSpaceCreate {
    cellappmgr::CreateSpaceRequest msg;
    Address src;
    Channel* ch;
  };
  std::vector<PendingSpaceCreate> pending_space_creates_awaiting_cellapps_;

  std::unordered_map<Address, Channel*> baseapps_;

  // EntityID high byte is app_id; app_id 0 remains invalid.
  uint32_t next_cellapp_app_id_{1};

  cellappmgr::CellID next_cell_id_{1};

  uint64_t last_balance_tick_{0};

  static constexpr float kBalanceSafetyBound = 0.9f;
  static constexpr uint64_t kBalanceTickInterval = 30;  // ~1 s @ 30 Hz
  static constexpr uint32_t kMaxCellAppAppId = 255;
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPPMGR_CELLAPPMGR_H_
