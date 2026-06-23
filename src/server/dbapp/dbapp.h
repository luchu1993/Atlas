#ifndef ATLAS_SERVER_DBAPP_DBAPP_H_
#define ATLAS_SERVER_DBAPP_DBAPP_H_

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "checkout_manager.h"
#include "db/database_factory.h"
#include "db/idatabase.h"
#include "dbapp_messages.h"
#include "dbappmgr/dbappmgr_messages.h"
#include "entity_id_allocator.h"
#include "entitydef/entity_def_registry.h"
#include "foundation/latency_histogram.h"
#include "network/machined_types.h"
#include "server/manager_app.h"

namespace atlas {

namespace login {
struct AuthLogin;
struct AuthLoginResult;
}  // namespace login

class DBApp : public ManagerApp {
 public:
  using ManagerApp::ManagerApp;

  static auto Run(int argc, char* argv[]) -> int;

 protected:
  auto Init(int argc, char* argv[]) -> bool override;
  void Fini() override;
  void OnTickComplete() override;
  void RegisterWatchers() override;

 private:
  friend class DBAppRollbackTest;

  void OnWriteEntity(const Address& src, Channel* ch, const dbapp::WriteEntity& msg);
  void OnCheckoutEntity(const Address& src, Channel* ch, const dbapp::CheckoutEntity& msg);
  void OnCheckinEntity(const Address& src, Channel* ch, const dbapp::CheckinEntity& msg);
  void OnDeleteEntity(const Address& src, Channel* ch, const dbapp::DeleteEntity& msg);
  void OnLookupEntity(const Address& src, Channel* ch, const dbapp::LookupEntity& msg);
  void OnAbortCheckout(const Address& src, Channel* ch, const dbapp::AbortCheckout& msg);

  void OnGetEntityIds(const Address& src, Channel* ch, const dbapp::GetEntityIds& msg);
  void OnPutEntityIds(const Address& src, Channel* ch, const dbapp::PutEntityIds& msg);

  void OnAuthLogin(const Address& src, Channel* ch, const login::AuthLogin& msg);

  void OnDbAppMgrBirth(const machined::BirthNotification& msg);
  void OnDbAppMgrDeath(const machined::DeathNotification& msg);
  void OnRegisterDbAppAck(const Address& src, Channel* ch, const dbappmgr::RegisterDbAppAck& msg);
  void OnShardTableUpdate(const Address& src, Channel* ch, const dbappmgr::ShardTableUpdate& msg);
  void OnBaseappDeath(const Address& internal_addr, std::string_view name, uint8_t reason);

  [[nodiscard]] auto BuildDbConfig() const -> DatabaseConfig;
  auto ResolveReplyChannel(const Address& addr) -> Channel*;
  void RegisterWithDbAppMgr();
  void ReportLoadToDbAppMgr();
  [[nodiscard]] auto CurrentLoadFraction() const -> float;
  [[nodiscard]] auto AcceptsShardTableVersion(uint32_t version) const -> bool;
  struct RequestCacheKey {
    Address reply_addr;
    uint32_t request_id{0};

    [[nodiscard]] auto operator==(const RequestCacheKey& other) const -> bool = default;
  };
  struct RequestCacheKeyHash {
    [[nodiscard]] auto operator()(const RequestCacheKey& key) const noexcept -> std::size_t;
  };
  [[nodiscard]] auto SendWriteAck(const RequestCacheKey& key, const dbapp::WriteEntityAck& ack,
                                  Channel* fallback_ch) -> bool;
  [[nodiscard]] auto SendCheckoutAck(const RequestCacheKey& key,
                                     const dbapp::CheckoutEntityAck& ack, Channel* fallback_ch)
      -> bool;
  void RememberWriteAck(const RequestCacheKey& key, const dbapp::WriteEntityAck& ack);
  void RememberCheckoutAck(const RequestCacheKey& key, const dbapp::CheckoutEntityAck& ack);
  [[nodiscard]] auto HasAuthoritativeShardTable() const -> bool;
  [[nodiscard]] auto FindShard(DatabaseID dbid) const -> const dbappmgr::ShardEntry*;
  [[nodiscard]] auto AllocateCreateDbid(const dbappmgr::ShardEntry& shard) -> DatabaseID;
  [[nodiscard]] auto AllocateCreateDbidForRoute(DatabaseID route_dbid) -> DatabaseID;
  [[nodiscard]] auto AllocateCreateDbidFromOwnedShard() -> DatabaseID;
  void PruneCreateDbidAllocators();

  std::unique_ptr<IDatabase> database_;
  std::unique_ptr<EntityIdAllocator> id_allocator_;
  CheckoutManager checkout_mgr_;
  struct PendingCheckoutRequest {
    DatabaseID dbid{kInvalidDBID};
    uint16_t type_id{0};
    Address reply_addr;
    bool canceled{false};
    DatabaseID cleared_dbid{kInvalidDBID};
  };
  std::unordered_map<RequestCacheKey, PendingCheckoutRequest, RequestCacheKeyHash>
      pending_checkout_requests_;
  std::unordered_set<RequestCacheKey, RequestCacheKeyHash> pending_write_requests_;
  std::unordered_map<RequestCacheKey, dbapp::WriteEntityAck, RequestCacheKeyHash> write_ack_cache_;
  std::deque<RequestCacheKey> write_ack_order_;
  std::unordered_map<RequestCacheKey, dbapp::CheckoutEntityAck, RequestCacheKeyHash>
      checkout_ack_cache_;
  std::deque<RequestCacheKey> checkout_ack_order_;
  std::optional<EntityDefRegistry> entity_defs_;  // nullopt until loaded
  bool auto_create_accounts_{false};
  uint16_t account_type_id_{0};
  uint64_t abort_checkout_total_{0};
  uint64_t abort_checkout_pending_hit_total_{0};
  uint64_t abort_checkout_late_hit_total_{0};
  Channel* dbappmgr_channel_{nullptr};
  uint32_t dbapp_id_{0};
  uint32_t shard_table_version_{0};
  std::vector<dbappmgr::ShardEntry> shard_table_;
  std::unordered_map<DatabaseID, DatabaseID> next_create_dbids_;
  TimePoint last_dbappmgr_load_report_at_{};

  LatencyHistogram checkout_reply_latency_;
  LatencyHistogram write_reply_latency_;
  static constexpr std::size_t kRequestAckCacheLimit = 4096;
};

}  // namespace atlas

#endif  // ATLAS_SERVER_DBAPP_DBAPP_H_
