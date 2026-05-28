#include <optional>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "baseapp/baseapp.h"
#include "coro/cancellation.h"
#include "db/idatabase.h"
#include "dbapp/dbapp.h"
#include "loginapp/loginapp.h"
#include "test_null_channel.h"

namespace atlas {

class LoginRollbackTest : public ::testing::Test {
 protected:
  LoginRollbackTest()
      : dispatcher_("login_rollback"),
        internal_network_(dispatcher_),
        external_network_(dispatcher_),
        app_(dispatcher_, internal_network_, external_network_) {}

  void register_watchers() { app_.RegisterWatchers(); }

  // Seed an active login's cancellation state (simulates what handle_login_coro sets up)
  auto seed_active_login(uint64_t channel_id, const std::string& username, uint32_t request_id)
      -> CancellationToken {
    CancellationSource source;
    auto token = source.Token();
    app_.channel_cancel_sources_[channel_id] = source;
    app_.pending_by_username_[username] = request_id;
    return token;
  }

  void cancel_for_channel(uint64_t channel_id) {
    auto it = app_.channel_cancel_sources_.find(channel_id);
    if (it != app_.channel_cancel_sources_.end()) it->second.RequestCancellation();
  }

  void add_pending_username(const std::string& username, uint32_t request_id) {
    app_.pending_by_username_[username] = request_id;
  }

  void remove_pending_username(const std::string& username) {
    app_.pending_by_username_.erase(username);
  }

  auto pending_by_username_empty() const -> bool { return app_.pending_by_username_.empty(); }
  auto active_login_count() const -> std::size_t { return app_.channel_cancel_sources_.size(); }
  auto abandoned_total() const -> uint64_t { return app_.abandoned_login_total_; }
  auto watcher(std::string_view path) -> std::optional<std::string> {
    return app_.GetWatcherRegistry().Get(path);
  }

  EventDispatcher dispatcher_;
  NetworkInterface internal_network_;
  NetworkInterface external_network_;
  LoginApp app_;
};

TEST_F(LoginRollbackTest, ClientDisconnectCancelsActiveLogin) {
  register_watchers();
  auto token = seed_active_login(100, "tester", 42);

  EXPECT_EQ(active_login_count(), 1u);
  EXPECT_FALSE(pending_by_username_empty());
  EXPECT_FALSE(token.IsCancelled());

  // Simulate what on_client_disconnect does: cancel the source
  cancel_for_channel(100);

  EXPECT_TRUE(token.IsCancelled());
}

TEST_F(LoginRollbackTest, DedupTrackingAndMetrics) {
  register_watchers();

  add_pending_username("alice", 1);
  add_pending_username("bob", 2);

  EXPECT_FALSE(pending_by_username_empty());
  EXPECT_EQ(watcher("loginapp/pending_logins").value_or(""), "2");

  remove_pending_username("alice");
  remove_pending_username("bob");
  EXPECT_TRUE(pending_by_username_empty());
  EXPECT_EQ(watcher("loginapp/pending_logins").value_or(""), "0");
}

class BaseAppRollbackTest : public ::testing::Test {
 protected:
  BaseAppRollbackTest()
      : dispatcher_("baseapp_rollback"),
        internal_network_(dispatcher_),
        external_network_(dispatcher_),
        app_(dispatcher_, internal_network_, external_network_) {}

  void register_watchers() { app_.RegisterWatchers(); }

  void seed_expired_prepared_login(uint32_t login_request_id, EntityID entity_id) {
    app_.prepared_login_entities_[login_request_id] = BaseApp::PreparedLoginEntity{
        entity_id, 1234, 7,
        Clock::now() - BaseApp::kPreparedLoginTimeout - std::chrono::seconds(1)};
    app_.prepared_login_requests_by_entity_[entity_id] = login_request_id;
  }

  void cleanup_expired() { app_.CleanupExpiredPendingRequests(); }

  auto has_prepared(uint32_t login_request_id) const -> bool {
    return app_.prepared_login_entities_.contains(login_request_id);
  }

  auto has_prepared_entity(EntityID entity_id) const -> bool {
    return app_.prepared_login_requests_by_entity_.contains(entity_id);
  }

  auto prepared_timeout_total() const -> uint64_t { return app_.prepared_login_timeout_total_; }
  auto watcher(std::string_view path) -> std::optional<std::string> {
    return app_.GetWatcherRegistry().Get(path);
  }

  auto create_cell_bound_entity(SpaceID space_id, const Address& cell_addr,
                                bool has_cell_backup = true) -> BaseEntity* {
    app_.entity_mgr_.SetIdClient(&app_.id_client_);
    if (!ids_seeded_) {
      app_.id_client_.AddIds(1000, 1100);
      ids_seeded_ = true;
    }
    auto* ent = app_.entity_mgr_.Create(/*type_id=*/7, /*has_client=*/false);
    if (ent == nullptr) return nullptr;
    ent->SetSpaceId(space_id);
    ent->SetCell(cell_addr);
    if (has_cell_backup) ent->SetCellBackupData({std::byte{0x01}});
    return ent;
  }

  auto death_notifications_total() const -> uint64_t {
    return app_.cellapp_death_notifications_total_;
  }
  auto death_restore_scheduled_total() const -> uint64_t {
    return app_.cellapp_death_restore_scheduled_total_;
  }
  auto death_restore_payload_scheduled_total() const -> uint64_t {
    return app_.cellapp_death_restore_payload_scheduled_total_;
  }
  auto death_restore_ghost_backup_scheduled_total() const -> uint64_t {
    return app_.cellapp_death_restore_ghost_backup_scheduled_total_;
  }
  auto death_restored_total() const -> uint64_t { return app_.cellapp_death_restored_total_; }
  auto death_lost_total() const -> uint64_t { return app_.cellapp_death_lost_total_; }
  auto death_restore_timeouts_total() const -> uint64_t {
    return app_.cellapp_death_restore_timeouts_total_;
  }
  auto death_restore_last_elapsed_ms() const -> uint64_t {
    return app_.cellapp_death_restore_last_elapsed_ms_;
  }
  auto death_restore_max_elapsed_ms() const -> uint64_t {
    return app_.cellapp_death_restore_max_elapsed_ms_;
  }
  auto pending_death_restore_count() const -> std::size_t {
    return app_.pending_cellapp_death_restores_.size();
  }
  void seed_death_restore_counters(uint64_t notifications, uint64_t scheduled,
                                   uint64_t restored, uint64_t lost, uint64_t timeouts) {
    app_.cellapp_death_notifications_total_ = notifications;
    app_.cellapp_death_restore_scheduled_total_ = scheduled;
    app_.cellapp_death_restore_payload_scheduled_total_ = scheduled;
    app_.cellapp_death_restore_ghost_backup_scheduled_total_ = 0;
    app_.cellapp_death_restored_total_ = restored;
    app_.cellapp_death_lost_total_ = lost;
    app_.cellapp_death_restore_timeouts_total_ = timeouts;
  }
  void seed_pending_death_restore(EntityID entity_id, TimePoint started_at = Clock::now()) {
    app_.pending_cellapp_death_restores_[entity_id] =
        BaseApp::PendingCellAppDeathRestore{started_at, Address(0x7F000001u, 30002)};
  }
  auto expired_death_restore_start() const -> TimePoint {
    return Clock::now() - BaseApp::kCellAppDeathRestoreTimeout - std::chrono::seconds(1);
  }
  void on_cell_entity_created(const baseapp::CellEntityCreated& msg) {
    app_.OnCellEntityCreated(*test_support::FakeChannel(0xCA11), msg);
  }
  void on_cell_entity_create_failed(const baseapp::CellEntityCreateFailed& msg) {
    app_.OnCellEntityCreateFailed(msg);
  }
  void on_cellapp_death(const baseapp::CellAppDeath& death) { app_.OnCellAppDeath(death); }
  void sweep_death_restore_timeouts() { app_.SweepCellAppDeathRestoreTimeouts(); }

  EventDispatcher dispatcher_;
  NetworkInterface internal_network_;
  NetworkInterface external_network_;
  BaseApp app_;
  bool ids_seeded_{false};
};

TEST_F(BaseAppRollbackTest, PreparedLoginTimeoutRollsBackPreparedState) {
  register_watchers();
  seed_expired_prepared_login(77, 901);

  cleanup_expired();

  EXPECT_FALSE(has_prepared(77));
  EXPECT_FALSE(has_prepared_entity(901));
  EXPECT_EQ(prepared_timeout_total(), 1u);
  EXPECT_EQ(watcher("baseapp/prepared_login_timeout_total").value_or(""), "1");
  EXPECT_EQ(watcher("baseapp/canceled_checkout_count").value_or(""), "0");
}

TEST_F(BaseAppRollbackTest, CellAppRoutesWatcherSummarizesBoundEntities) {
  register_watchers();
  const Address addr_a(0x7F000001u, 30001);
  const Address addr_b(0x7F000001u, 30002);
  ASSERT_NE(create_cell_bound_entity(42, addr_a), nullptr);
  ASSERT_NE(create_cell_bound_entity(42, addr_a, /*has_cell_backup=*/false), nullptr);
  ASSERT_NE(create_cell_bound_entity(42, addr_b, /*has_cell_backup=*/false), nullptr);

  const auto summary = watcher("baseapp/cellapp_routes").value_or("");
  const auto addr_a_entry = "addr=" + addr_a.ToString() +
                            " entities=2 payload_candidates=1 ghost_backup_candidates=1";
  const auto addr_b_entry = "addr=" + addr_b.ToString() +
                            " entities=1 payload_candidates=0 ghost_backup_candidates=1";
  EXPECT_NE(summary.find("routes=2"), std::string::npos);
  EXPECT_NE(summary.find(addr_a_entry), std::string::npos);
  EXPECT_NE(summary.find(addr_b_entry), std::string::npos);
}

TEST_F(BaseAppRollbackTest, CellAppDeathLostMetricsExposeWatcherCounters) {
  register_watchers();
  const Address dead_addr(0x7F000001u, 30001);
  auto* ent = create_cell_bound_entity(42, dead_addr);
  ASSERT_NE(ent, nullptr);

  baseapp::CellAppDeath death;
  death.dead_addr = dead_addr;
  on_cellapp_death(death);

  EXPECT_FALSE(ent->HasCell());
  EXPECT_EQ(death_notifications_total(), 1u);
  EXPECT_EQ(death_restore_scheduled_total(), 0u);
  EXPECT_EQ(death_restore_payload_scheduled_total(), 0u);
  EXPECT_EQ(death_restore_ghost_backup_scheduled_total(), 0u);
  EXPECT_EQ(death_lost_total(), 1u);
  EXPECT_EQ(watcher("baseapp/cellapp_death_notifications_total").value_or(""), "1");
  EXPECT_EQ(watcher("baseapp/cellapp_death_restore_scheduled_total").value_or(""), "0");
  EXPECT_EQ(watcher("baseapp/cellapp_death_restore_payload_scheduled_total").value_or(""),
            "0");
  EXPECT_EQ(
      watcher("baseapp/cellapp_death_restore_ghost_backup_scheduled_total").value_or(""),
      "0");
  EXPECT_EQ(watcher("baseapp/cellapp_death_restored_total").value_or(""), "0");
  EXPECT_EQ(watcher("baseapp/cellapp_death_lost_total").value_or(""), "1");
  EXPECT_EQ(watcher("baseapp/cellapp_death_restore_last_elapsed_ms").value_or(""), "0");
  EXPECT_EQ(watcher("baseapp/cellapp_death_restore_max_elapsed_ms").value_or(""), "0");
  EXPECT_EQ(watcher("baseapp/cellapp_death_restore_status").value_or(""),
            "state=degraded notifications=1 scheduled=0 payload_scheduled=0 "
            "ghost_backup_scheduled=0 restored=0 lost=1 timeouts=0 pending=0 "
            "unresolved=0 last_elapsed_ms=0 max_elapsed_ms=0");
}

TEST_F(BaseAppRollbackTest, CellAppDeathRestoreStatusExposesUnresolvedScheduledWork) {
  register_watchers();
  seed_death_restore_counters(/*notifications=*/1, /*scheduled=*/2, /*restored=*/1,
                              /*lost=*/0, /*timeouts=*/0);

  EXPECT_EQ(watcher("baseapp/cellapp_death_restore_status").value_or(""),
            "state=degraded notifications=1 scheduled=2 payload_scheduled=2 "
            "ghost_backup_scheduled=0 restored=1 lost=0 timeouts=0 pending=0 "
            "unresolved=1 last_elapsed_ms=0 max_elapsed_ms=0");
}

TEST_F(BaseAppRollbackTest, CellEntityCreatedCompletesPendingDeathRestore) {
  register_watchers();
  const Address dead_addr(0x7F000001u, 30001);
  auto* ent = create_cell_bound_entity(42, dead_addr);
  ASSERT_NE(ent, nullptr);
  ent->ClearCell();
  seed_pending_death_restore(ent->EntityId());

  baseapp::CellEntityCreated created;
  created.entity_id = ent->EntityId();
  created.cell_addr = Address(0x7F000001u, 30002);
  on_cell_entity_created(created);

  EXPECT_TRUE(ent->HasCell());
  EXPECT_EQ(death_restored_total(), 1u);
  EXPECT_EQ(death_lost_total(), 0u);
  EXPECT_EQ(pending_death_restore_count(), 0u);
  EXPECT_EQ(watcher("baseapp/cellapp_death_restored_total").value_or(""), "1");
}

TEST_F(BaseAppRollbackTest, CellEntityCreateFailedCompletesPendingDeathRestoreAsLost) {
  register_watchers();
  const Address dead_addr(0x7F000001u, 30001);
  auto* ent = create_cell_bound_entity(42, dead_addr);
  ASSERT_NE(ent, nullptr);
  ent->ClearCell();
  seed_pending_death_restore(ent->EntityId());

  baseapp::CellEntityCreateFailed failed;
  failed.entity_id = ent->EntityId();
  failed.request_id = ent->EntityId();
  failed.reason = baseapp::CellEntityCreateFailureReason::kGhostRequiredMissing;
  on_cell_entity_create_failed(failed);

  EXPECT_FALSE(ent->HasCell());
  EXPECT_EQ(death_restored_total(), 0u);
  EXPECT_EQ(death_lost_total(), 1u);
  EXPECT_EQ(pending_death_restore_count(), 0u);
  EXPECT_EQ(watcher("baseapp/cellapp_death_lost_total").value_or(""), "1");
}

TEST_F(BaseAppRollbackTest, CellAppDeathRestoreTimeoutCompletesPendingAsLost) {
  register_watchers();
  const Address dead_addr(0x7F000001u, 30001);
  auto* ent = create_cell_bound_entity(42, dead_addr);
  ASSERT_NE(ent, nullptr);
  ent->ClearCell();
  seed_pending_death_restore(ent->EntityId(), expired_death_restore_start());

  sweep_death_restore_timeouts();

  EXPECT_FALSE(ent->HasCell());
  EXPECT_EQ(death_restored_total(), 0u);
  EXPECT_EQ(death_lost_total(), 1u);
  EXPECT_EQ(death_restore_timeouts_total(), 1u);
  EXPECT_EQ(pending_death_restore_count(), 0u);
  EXPECT_GE(death_restore_last_elapsed_ms(), 5000u);
  EXPECT_EQ(death_restore_max_elapsed_ms(), death_restore_last_elapsed_ms());
  EXPECT_EQ(watcher("baseapp/cellapp_death_lost_total").value_or(""), "1");
  EXPECT_EQ(watcher("baseapp/cellapp_death_restore_timeouts_total").value_or(""), "1");
  EXPECT_EQ(watcher("baseapp/cellapp_death_pending_restores").value_or(""), "0");
  EXPECT_EQ(watcher("baseapp/cellapp_death_restore_last_elapsed_ms").value_or(""),
            std::to_string(death_restore_last_elapsed_ms()));
  EXPECT_EQ(watcher("baseapp/cellapp_death_restore_max_elapsed_ms").value_or(""),
            std::to_string(death_restore_max_elapsed_ms()));
}

class FakeDatabase final : public IDatabase {
 public:
  Result<void> Startup(const DatabaseConfig&, const EntityDefRegistry&) override { return {}; }
  void Shutdown() override {}

  void PutEntity(DatabaseID, uint16_t, WriteFlags, std::span<const std::byte>, const std::string&,
                 std::function<void(PutResult)>) override {
    ADD_FAILURE() << "PutEntity should not be called in this test";
  }

  void GetEntity(DatabaseID, uint16_t, std::function<void(GetResult)>) override {
    ADD_FAILURE() << "GetEntity should not be called in this test";
  }

  void DelEntity(DatabaseID, uint16_t, std::function<void(DelResult)>) override {
    ADD_FAILURE() << "DelEntity should not be called in this test";
  }

  void LookupByName(uint16_t, const std::string&, std::function<void(LookupResult)>) override {
    ADD_FAILURE() << "LookupByName should not be called in this test";
  }

  void CheckoutEntity(DatabaseID, uint16_t, const CheckoutInfo&,
                      std::function<void(GetResult)> callback) override {
    checkout_callback = std::move(callback);
  }

  void CheckoutEntityByName(uint16_t, const std::string&, const CheckoutInfo&,
                            std::function<void(GetResult)> callback) override {
    checkout_callback = std::move(callback);
  }

  void ClearCheckout(DatabaseID dbid, uint16_t type_id,
                     std::function<void(bool)> callback) override {
    ++clear_checkout_calls;
    last_cleared = std::make_pair(dbid, type_id);
    callback(true);
  }

  void ClearCheckoutsForAddress(const Address&, std::function<void(int)> callback) override {
    callback(0);
  }

  void MarkCheckoutCleared(DatabaseID dbid, uint16_t type_id) override {
    ++mark_checkout_cleared_calls;
    last_cleared = std::make_pair(dbid, type_id);
  }

  void GetAutoLoadEntities(std::function<void(std::vector<EntityData>)> callback) override {
    callback({});
  }

  void SetAutoLoad(DatabaseID, uint16_t, bool) override {}
  void LoadEntityIdCounter(std::function<void(EntityID)> callback) override { callback(1); }
  void SaveEntityIdCounter(EntityID, std::function<void(bool)> callback) override {
    callback(true);
  }
  void ProcessResults() override {}

  std::function<void(GetResult)> checkout_callback;
  int clear_checkout_calls{0};
  int mark_checkout_cleared_calls{0};
  std::optional<std::pair<DatabaseID, uint16_t>> last_cleared;
};

class DBAppRollbackTest : public ::testing::Test {
 protected:
  DBAppRollbackTest()
      : dispatcher_("dbapp_rollback"), network_(dispatcher_), app_(dispatcher_, network_) {
    app_.database_ = std::make_unique<FakeDatabase>();
  }

  auto db() -> FakeDatabase& { return *static_cast<FakeDatabase*>(app_.database_.get()); }

  void register_watchers() { app_.RegisterWatchers(); }

  void start_checkout(uint32_t request_id, DatabaseID dbid, uint16_t type_id) {
    dbapp::CheckoutEntity checkout;
    checkout.request_id = request_id;
    checkout.dbid = dbid;
    checkout.type_id = type_id;
    app_.OnCheckoutEntity(Address("127.0.0.1", 30001), reinterpret_cast<Channel*>(1), checkout);
  }

  void abort_checkout(uint32_t request_id, DatabaseID dbid, uint16_t type_id) {
    dbapp::AbortCheckout abort;
    abort.request_id = request_id;
    abort.dbid = dbid;
    abort.type_id = type_id;
    app_.OnAbortCheckout(Address("127.0.0.1", 30001), reinterpret_cast<Channel*>(1), abort);
  }

  void complete_checkout_success(DatabaseID dbid) {
    ASSERT_TRUE(db().checkout_callback);
    GetResult result;
    result.success = true;
    result.data.dbid = dbid;
    db().checkout_callback(std::move(result));
  }

  auto pending_request_contains(uint32_t request_id) const -> bool {
    return app_.pending_checkout_requests_.contains(request_id);
  }

  auto pending_request_canceled(uint32_t request_id) const -> bool {
    return app_.pending_checkout_requests_.at(request_id).canceled;
  }

  auto pending_request_cleared_dbid(uint32_t request_id) const -> DatabaseID {
    return app_.pending_checkout_requests_.at(request_id).cleared_dbid;
  }

  auto checkout_owner(DatabaseID dbid, uint16_t type_id) const -> std::optional<CheckoutInfo> {
    return app_.checkout_mgr_.GetOwner(dbid, type_id);
  }

  auto abort_total() const -> uint64_t { return app_.abort_checkout_total_; }
  auto abort_pending_total() const -> uint64_t { return app_.abort_checkout_pending_hit_total_; }
  auto abort_late_total() const -> uint64_t { return app_.abort_checkout_late_hit_total_; }
  auto watcher(std::string_view path) -> std::optional<std::string> {
    return app_.GetWatcherRegistry().Get(path);
  }

  EventDispatcher dispatcher_;
  NetworkInterface network_;
  DBApp app_;
};

TEST_F(DBAppRollbackTest, AbortCheckoutIsIdempotentWhileRequestIsPending) {
  register_watchers();
  start_checkout(1001, 555, 9);
  ASSERT_TRUE(db().checkout_callback);
  ASSERT_TRUE(pending_request_contains(1001));

  abort_checkout(1001, 555, 9);
  abort_checkout(1001, 555, 9);

  EXPECT_EQ(db().mark_checkout_cleared_calls, 1);
  ASSERT_TRUE(pending_request_contains(1001));
  EXPECT_TRUE(pending_request_canceled(1001));
  EXPECT_EQ(pending_request_cleared_dbid(1001), 555);

  complete_checkout_success(555);

  EXPECT_FALSE(pending_request_contains(1001));
  EXPECT_EQ(db().mark_checkout_cleared_calls, 1);
  EXPECT_FALSE(checkout_owner(555, 9).has_value());
  EXPECT_EQ(abort_total(), 2u);
  EXPECT_EQ(abort_pending_total(), 2u);
  EXPECT_EQ(abort_late_total(), 0u);
  EXPECT_EQ(watcher("dbapp/abort_checkout_total").value_or(""), "2");
}

}  // namespace atlas
