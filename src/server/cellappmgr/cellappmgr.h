#ifndef ATLAS_SERVER_CELLAPPMGR_CELLAPPMGR_H_
#define ATLAS_SERVER_CELLAPPMGR_CELLAPPMGR_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "bsp_tree.h"
#include "cellappmgr_messages.h"
#include "foundation/clock.h"
#include "server/entity_types.h"
#include "server/manager_app.h"

namespace atlas {

class Channel;
namespace machined {
struct ProcessInfo;
}

class CellAppMgr : public ManagerApp {
 public:
  static auto Run(int argc, char* argv[]) -> int;

  CellAppMgr(EventDispatcher& dispatcher, NetworkInterface& network);

  void OnRegisterCellApp(const Address& src, Channel* ch, const cellappmgr::RegisterCellApp& msg);
  void OnInformCellLoad(const Address& src, Channel* ch, const cellappmgr::InformCellLoad& msg);
  void OnCreateSpaceRequest(const Address& src, Channel* ch,
                            const cellappmgr::CreateSpaceRequest& msg);
  void OnAddCellToSpaceAck(const Address& src, Channel* ch,
                           const cellappmgr::AddCellToSpaceAck& msg);
  void OnHealthProbe(const Address& src, Channel* ch, const cellappmgr::HealthProbe& msg);
  void OnRecoverCellAppState(const Address& src, Channel* ch,
                             const cellappmgr::RecoverCellAppState& msg);

  void OnCellAppDeath(const Address& internal_addr, uint8_t reason);

  struct CellAppInfo {
    Address internal_addr;
    uint32_t app_id{0};
    float load{0.f};
    uint32_t entity_count{0};
    Channel* channel{nullptr};
    TimePoint registered_at{};
    TimePoint last_load_report_at{};
    bool is_retiring{false};
  };

  struct SpacePartition {
    SpaceID space_id{kInvalidSpaceID};
    BSPTree bsp;
    uint64_t geometry_version{0};
    uint64_t freeze_epoch{0};
    // Last fan-out bytes; stable trees skip redundant broadcasts.
    std::vector<std::byte> last_broadcast_blob;
    std::vector<std::byte> last_debug_geometry_blob;
    std::size_t last_debug_geometry_baseapp_count{0};
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
  void BackdateCellAppRegistrationForTest(const Address& addr, Duration age) {
    if (auto it = cellapps_.find(addr); it != cellapps_.end()) {
      it->second.registered_at = Clock::now() - age;
    }
  }

  void TickLoadBalance();

 protected:
  [[nodiscard]] auto Init(int argc, char* argv[]) -> bool override;
  void Fini() override;
  void RegisterWatchers() override;
  void OnTickComplete() override;

 private:
  struct RetireDrain;
  struct LbDecision;
  struct LeafTopologySnapshot {
    cellappmgr::CellID cell_id{0};
    uint32_t app_id{0};
    float load{0.f};
    uint32_t entity_count{0};
    CellBounds bounds;
  };

  [[nodiscard]] auto BuildCellAppLoadSummary() const -> std::string;
  [[nodiscard]] auto BuildSpaceLoadSummary() const -> std::string;
  [[nodiscard]] auto BuildSpaceLoadSummary(const SpacePartition& partition) const -> std::string;
  [[nodiscard]] auto TopologyPendingAckCount() const -> std::size_t;
  [[nodiscard]] auto BuildTopologyFingerprint() const -> std::string;
  [[nodiscard]] auto BuildPendingSpaceCreateSummary() const -> std::string;
  [[nodiscard]] auto BuildRetireStatusSummary() const -> std::string;
  [[nodiscard]] auto RecoveryWindowActive() const -> bool;
  [[nodiscard]] auto BuildLbDecisionSummary() const -> std::string;
  [[nodiscard]] auto BuildLbDecisionHistorySummary() const -> std::string;
  [[nodiscard]] auto FormatLbDecision(const LbDecision& decision) const -> std::string;
  [[nodiscard]] auto SnapshotLeafTopology(const SpacePartition& partition,
                                          const Address& app_id_override_addr = Address{},
                                          uint32_t app_id_override = 0) const
      -> std::vector<LeafTopologySnapshot>;
  [[nodiscard]] static auto FormatBoundsForDecision(const CellBounds& bounds) -> std::string;
  [[nodiscard]] static auto LeafTopologyEqual(const LeafTopologySnapshot& a,
                                              const LeafTopologySnapshot& b) -> bool;
  [[nodiscard]] static auto FormatLeafTopologyChange(const LeafTopologySnapshot* before,
                                                     const LeafTopologySnapshot* after)
      -> std::string;
  [[nodiscard]] auto AppendTopologyDiff(std::string detail, const SpacePartition& partition,
                                        uint64_t before_version,
                                        const std::vector<LeafTopologySnapshot>& before) const
      -> std::string;
  [[nodiscard]] auto AppIdForAddress(const Address& addr) const -> uint32_t;
  [[nodiscard]] auto AssignableCellAppCount() const -> std::size_t;
  [[nodiscard]] auto StaleLoadReportCount() const -> std::size_t;
  [[nodiscard]] auto RetiringCellAppCount() const -> std::size_t;
  [[nodiscard]] auto RetireDrainCount() const -> std::size_t;
  [[nodiscard]] auto RetireStuckDrainCount() const -> std::size_t;
  [[nodiscard]] auto RetiringAppIdForWatcher() const -> uint32_t;
  [[nodiscard]] auto SetRetiringAppId(uint32_t app_id) -> bool;
  void RecordLbDecision(std::string action, std::string reason, SpaceID space_id,
                        cellappmgr::CellID cell_id, cellappmgr::CellID target_cell_id,
                        uint32_t source_app_id, uint32_t target_app_id,
                        uint64_t geometry_version, std::string detail);
  [[nodiscard]] auto HandleRetireDrainReport(
      const Address& source_addr, const cellappmgr::InformCellLoad::CellReport& rep) -> bool;
  [[nodiscard]] auto IsRetireDrainStuck(const RetireDrain& drain, TimePoint now) const -> bool;
  // Sorted ascending by (load, app_id) for deterministic multi-cell
  // bootstrap; retiring CellApps are excluded.
  [[nodiscard]] auto SortedHostsForBootstrap(std::size_t max) const
      -> std::vector<const CellAppInfo*>;

  // BFS N-1 splits with alternating X/Z axes; N=4 lands as a 2x2 grid.
  // Each new leaf takes hosts[i] in order; partial bootstrap on Split error.
  void BootstrapMultiCellPartition(SpacePartition& partition,
                                   const std::vector<const CellAppInfo*>& hosts);

  // Splits the heaviest leaf in every Space whose leaf count is below the
  // assignable CellApp count, assigning the new half to `new_app`.
  void GrowSpacesForNewCellApp(const CellAppInfo& new_app);
  [[nodiscard]] auto TryAutoSplitHotLeaf(SpacePartition& partition) -> bool;
  [[nodiscard]] auto TryAutoMergeIdleLeaf(SpacePartition& partition) -> bool;
  [[nodiscard]] auto TryRetireOneLeaf(SpacePartition& partition) -> bool;
  [[nodiscard]] auto TryRetireHandoffLeaf(SpacePartition& partition) -> bool;
  [[nodiscard]] auto SplitLeafToHost(SpacePartition& partition, const CellInfo& target,
                                     const CellAppInfo& new_app, const char* reason) -> bool;
  [[nodiscard]] auto PickHeaviestLeaf(const SpacePartition& partition) const -> const CellInfo*;
  [[nodiscard]] auto PickIdleHostForAutoSplit(const SpacePartition& partition) const
      -> const CellAppInfo*;
  [[nodiscard]] auto PickRetireDrainTarget(const SpacePartition& partition,
                                           const CellInfo& source_leaf) const
      -> const CellAppInfo*;
  [[nodiscard]] auto HasPendingGeometryBroadcast(SpaceID space_id) const -> bool;
  struct MergeCandidate {
    cellappmgr::CellID remove_cell_id{0};
    cellappmgr::CellID keep_cell_id{0};
    Address remove_addr;
  };
  [[nodiscard]] auto PickMergeCandidate(const SpacePartition& partition)
      -> std::optional<MergeCandidate>;

  // Timed-out pending broadcasts fall back to broadcasting anyway so a
  // dead/slow receiver doesn't stall the cluster.
  void DrainPendingGeometryBroadcasts();
  void AuditRetireDrainWatchdog();
  void MarkRetireDrainGeometryPublished(SpaceID space_id, cellappmgr::CellID cell_id,
                                        const Address& target_addr);
  void SendRegisterCellAppAck(Channel* ch, const Address& addr, uint32_t app_id, bool success,
                              const char* context);

  void SendCreateSpaceReply(const cellappmgr::CreateSpaceRequest& msg, const Address& src,
                            Channel* ch, bool ok, cellappmgr::CellID cell_id, Address host_addr);
  void ExecuteCreateSpace(const cellappmgr::CreateSpaceRequest& msg, const Address& src,
                          Channel* ch);
  void DrainExpiredCreateSpaceRequests();

  [[nodiscard]] auto PickAlternateHost(const Address& exclude_addr) const -> const CellAppInfo*;

  // Prefers a survivor that already holds a leaf of this space — its
  // ghost SpaceData replica is up to date, ideal for owner handoff.
  [[nodiscard]] auto PickAlternateHostInSpace(const Address& exclude_addr,
                                              const SpacePartition& partition) const
      -> const CellAppInfo*;

  void SendAddCell(const CellAppInfo& target, SpaceID space_id, cellappmgr::CellID cell_id,
                   const CellBounds& bounds, bool is_primary,
                   const std::string& space_master_type,
                   const Address& space_data_source_addr = {});
  void SendRemoveCell(const CellAppInfo& target, SpaceID space_id, cellappmgr::CellID cell_id);
  struct ExtraGeometryRecipient {
    Address addr;
    cellappmgr::CellID cell_id{0};
  };
  void BroadcastGeometry(SpacePartition& partition,
                         std::span<const ExtraGeometryRecipient> extra_recipients = {});

  std::unordered_map<Address, CellAppInfo> cellapps_;
  std::unordered_map<SpaceID, SpacePartition> spaces_;
  uint32_t last_retire_app_id_{0};
  struct RetireDrain {
    SpaceID space_id{kInvalidSpaceID};
    cellappmgr::CellID cell_id{0};
    Address source_addr;
    Address target_addr;
    uint32_t last_entity_count{0};
    TimePoint started_at{};
    TimePoint last_progress_at{};
    TimePoint last_watchdog_log_at{};
    bool geometry_published{false};
  };
  std::vector<RetireDrain> retire_drains_;
  struct LbDecision {
    uint64_t sequence{0};
    uint64_t tick{0};
    std::string action{"none"};
    std::string reason{"none"};
    SpaceID space_id{kInvalidSpaceID};
    cellappmgr::CellID cell_id{0};
    cellappmgr::CellID target_cell_id{0};
    uint32_t source_app_id{0};
    uint32_t target_app_id{0};
    uint64_t geometry_version{0};
    std::string detail;
  };
  LbDecision last_lb_decision_;
  std::vector<LbDecision> lb_decision_history_;
  uint64_t lb_decision_count_{0};
  // Per-cell entity stats from the latest InformCellLoad; drives heaviest-
  // leaf pick and bucket/median Split position for elastic split.
  struct CellDistribution {
    uint32_t entity_count{0};
    float median_x{0.f};
    float median_z{0.f};
    float weighted_load{0.f};
    float tick_load{0.f};
    uint64_t script_tick_us{0};
    uint64_t native_tick_us{0};
    uint32_t witness_count{0};
    uint32_t aoi_peer_count{0};
    uint64_t aoi_reliable_bytes{0};
    uint64_t aoi_unreliable_bytes{0};
    uint64_t backup_bytes{0};
    std::array<uint32_t, cellappmgr::InformCellLoad::CellReport::kLoadBucketCount> x_buckets{};
    std::array<uint32_t, cellappmgr::InformCellLoad::CellReport::kLoadBucketCount> z_buckets{};
    std::array<uint64_t, cellappmgr::InformCellLoad::CellReport::kLoadBucketCount>
        x_load_buckets{};
    std::array<uint64_t, cellappmgr::InformCellLoad::CellReport::kLoadBucketCount>
        z_load_buckets{};
  };
  std::unordered_map<cellappmgr::CellID, CellDistribution> cell_distributions_;
  std::unordered_map<cellappmgr::CellID, uint32_t> hot_leaf_balance_ticks_;
  std::unordered_map<cellappmgr::CellID, uint32_t> idle_leaf_balance_ticks_;
  // Each new registration extends quiescence_deadline so a stagger-launched
  // cluster bootstraps all N cellapps at once.
  struct PendingSpaceCreate {
    cellappmgr::CreateSpaceRequest msg;
    Address src;
    Channel* ch;
    TimePoint queued_at;
    TimePoint quiescence_deadline;
  };
  std::vector<PendingSpaceCreate> pending_space_creates_awaiting_cellapps_;

  // Defers topology broadcasts until the new host acks AddCellToSpace.
  // Primary handoff can disable timeout fallback until SpaceData is copied.
  struct PendingGeometryBroadcast {
    SpaceID space_id;
    cellappmgr::CellID awaiting_cell_id;
    Address awaiting_addr;
    TimePoint sent_at;
    std::vector<ExtraGeometryRecipient> extra_recipients;
    bool allow_timeout_broadcast{true};
  };
  std::vector<PendingGeometryBroadcast> pending_geometry_broadcasts_;

  std::unordered_map<Address, Channel*> baseapps_;

  // EntityID high byte is app_id; app_id 0 remains invalid.
  uint32_t next_cellapp_app_id_{1};

  cellappmgr::CellID next_cell_id_{1};

  uint64_t last_balance_tick_{0};

  static constexpr float kBalanceSafetyBound = 0.9f;
  static constexpr uint64_t kBalanceTickInterval = 30;  // ~1 s @ 30 Hz
  static constexpr uint32_t kMaxCellAppAppId = 255;
  static constexpr std::size_t kLbDecisionHistoryLimit = 16;
  static constexpr std::size_t kLbDecisionLeafDiffLimit = 8;
  // Root world half-extent. Finite bounds keep recursive midpoint splits
  // strictly-inside; ±inf trips BSPTree::Split's bounds check at depth 2.
  static constexpr float kDefaultWorldHalfExtent = 1000.f;
  // Past this window we broadcast geometry without the ack; OnCellAppDeath
  // will rehome the leaf if the receiver is actually dead.
  static constexpr Duration kPendingGeometryTimeout = Milliseconds(500);
  static constexpr Duration kStartupQuiescenceWindowDefault =
      std::chrono::duration_cast<Duration>(std::chrono::seconds(2));
  Duration startup_quiescence_window_{kStartupQuiescenceWindowDefault};
  // Set in Init from the startup window; restore gate stays closed until then
  // so worker BSP reports can rebuild topology first. Unset = open.
  TimePoint recovery_deadline_{};

 public:
  void SetStartupQuiescenceWindowForTest(Duration w) {
    startup_quiescence_window_ = w;
    // Keep the recovery deadline consistent so callers that shrink the window
    // after Init (integration fixtures) also reopen the restore gate.
    recovery_deadline_ = w > Duration::zero() ? Clock::now() + w : TimePoint{};
  }
  void SetRecoveryDeadlineForTest(TimePoint t) { recovery_deadline_ = t; }
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPPMGR_CELLAPPMGR_H_
