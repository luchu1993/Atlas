#include <array>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "baseapp/baseapp.h"
#include "coro/cancellation.h"
#include "db/idatabase.h"
#include "dbapp/dbapp.h"
#include "dbappmgr/dbappmgr_messages.h"
#include "loginapp/loginapp.h"
#include "serialization/binary_stream.h"
#include "server/db_shard_routing.h"
#include "test_null_channel.h"

namespace atlas {

namespace {

class CapturingChannel final : public Channel {
 public:
  using Channel::Channel;

  [[nodiscard]] auto Fd() const -> FdHandle override { return kInvalidFd; }
  [[nodiscard]] auto send_count() const -> std::size_t { return sends_.size(); }
  [[nodiscard]] auto Sends() const -> const std::vector<std::vector<std::byte>>& { return sends_; }

 protected:
  [[nodiscard]] auto DoSend(std::span<const std::byte> data) -> Result<std::size_t> override {
    sends_.emplace_back(data.begin(), data.end());
    return data.size();
  }

 private:
  std::vector<std::vector<std::byte>> sends_;
};

template <typename Msg>
auto DecodeSentMessages(const CapturingChannel& ch) -> std::vector<Msg> {
  std::vector<Msg> out;
  for (const auto& frame : ch.Sends()) {
    BinaryReader reader(std::span<const std::byte>(frame.data(), frame.size()));
    const auto id = reader.ReadPackedInt();
    if (!id || *id != Msg::Descriptor().id) continue;

    if (Msg::Descriptor().length_style == MessageLengthStyle::kVariable) {
      const auto len = reader.ReadPackedInt();
      if (!len) continue;
      const auto payload = reader.ReadBytes(*len);
      if (!payload) continue;
      BinaryReader msg_reader(*payload);
      auto msg = Msg::Deserialize(msg_reader);
      if (msg.HasValue()) out.push_back(std::move(*msg));
      continue;
    }

    auto msg = Msg::Deserialize(reader);
    if (msg.HasValue()) out.push_back(std::move(*msg));
  }
  return out;
}

}  // namespace

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
  void set_legacy_dbapp_channel(Channel* ch) { app_.dbapp_channel_ = ch; }
  void cache_dbapp_channel(const Address& addr, Channel* ch) { app_.dbapp_channels_[addr] = ch; }
  auto cached_dbapp_channel_count() const -> std::size_t { return app_.dbapp_channels_.size(); }
  void apply_dbapp_shards(uint32_t version, std::vector<dbappmgr::ShardEntry> entries) {
    app_.ApplyDbAppShardTable(version, std::move(entries));
  }
  auto resolve_auth_dbapp(std::string_view username) -> Channel* {
    return app_.ResolveAuthDbAppChannel(username);
  }
  void handle_dbapp_death(const Address& addr) {
    machined::DeathNotification death;
    death.process_type = ProcessType::kDbApp;
    death.internal_addr = addr;
    death.reason = 1;
    app_.OnDbAppDeath(death);
  }
  auto start_login(uint64_t client_channel_id, std::string username) -> uint32_t {
    login::LoginRequest request;
    request.username = std::move(username);
    request.password_hash = "pw";
    const uint32_t request_id = app_.next_request_id_;
    app_.HandleLoginCoro(client_channel_id, Address(0x7F000001u, 26001), std::move(request));
    return request_id;
  }
  void handle_auth_result(const login::AuthLoginResult& result) {
    BinaryWriter writer;
    result.Serialize(writer);
    ASSERT_TRUE(
        app_.rpc_registry_.TryDispatch(login::AuthLoginResult::Descriptor().id, writer.Data()));
  }
  template <typename Predicate>
  auto drive_until(Predicate predicate, Duration timeout) -> bool {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
      dispatcher_.ProcessOnce();
      if (predicate()) return true;
    }
    dispatcher_.ProcessOnce();
    return predicate();
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

TEST_F(LoginRollbackTest, AuthLoginUsesLegacyDbappWhenShardTableMissing) {
  auto* legacy = test_support::FakeChannel(0x1501);
  set_legacy_dbapp_channel(legacy);

  EXPECT_EQ(resolve_auth_dbapp("alice"), legacy);
}

TEST_F(LoginRollbackTest, AuthLoginRoutesUsernameThroughShardTable) {
  register_watchers();
  const Address shard_a_addr("127.0.0.1", 31001);
  const Address shard_b_addr("127.0.0.1", 31002);
  auto* shard_a = test_support::FakeChannel(0x1502);
  auto* shard_b = test_support::FakeChannel(0x1503);
  cache_dbapp_channel(shard_a_addr, shard_a);
  cache_dbapp_channel(shard_b_addr, shard_b);

  const DatabaseID kSplit = std::numeric_limits<DatabaseID>::max() / 2;
  apply_dbapp_shards(
      3, {dbappmgr::ShardEntry{1, kSplit, 1, shard_a_addr},
          dbappmgr::ShardEntry{kSplit, std::numeric_limits<DatabaseID>::max(), 2, shard_b_addr}});

  std::string routed_to_a;
  std::string routed_to_b;
  for (int i = 0; i < 10000 && (routed_to_a.empty() || routed_to_b.empty()); ++i) {
    std::string username = std::format("user{}", i);
    Channel* target = resolve_auth_dbapp(username);
    if (target == shard_a && routed_to_a.empty()) routed_to_a = username;
    if (target == shard_b && routed_to_b.empty()) routed_to_b = username;
  }

  ASSERT_FALSE(routed_to_a.empty());
  ASSERT_FALSE(routed_to_b.empty());
  EXPECT_EQ(resolve_auth_dbapp(routed_to_a), shard_a);
  EXPECT_EQ(resolve_auth_dbapp(routed_to_b), shard_b);
  EXPECT_EQ(watcher("loginapp/dbapp_shard_table_version").value_or(""), "3");
  EXPECT_EQ(watcher("loginapp/dbapp_shard_count").value_or(""), "2");
}

TEST_F(LoginRollbackTest, DbappDeathClearsCachedShardChannel) {
  const Address shard_addr("127.0.0.1", 31003);
  cache_dbapp_channel(shard_addr, test_support::FakeChannel(0x1504));
  EXPECT_EQ(cached_dbapp_channel_count(), 1u);

  handle_dbapp_death(shard_addr);

  EXPECT_EQ(cached_dbapp_channel_count(), 0u);
}

TEST_F(LoginRollbackTest, AuthLoginRetriesAfterDbappDeathAndShardUpdate) {
  InterfaceTable channel_table;
  CapturingChannel old_shard(dispatcher_, channel_table, Address(0x7F000001u, 31004));
  CapturingChannel new_shard(dispatcher_, channel_table, Address(0x7F000001u, 31005));
  const Address old_addr(0x7F000001u, 31004);
  const Address new_addr(0x7F000001u, 31005);

  cache_dbapp_channel(old_addr, &old_shard);
  apply_dbapp_shards(
      7, {dbappmgr::ShardEntry{1, std::numeric_limits<DatabaseID>::max(), 8, old_addr}});

  const uint32_t request_id = start_login(77, "alice");

  const auto first_auths = DecodeSentMessages<login::AuthLogin>(old_shard);
  ASSERT_EQ(first_auths.size(), 1u);
  EXPECT_EQ(first_auths[0].request_id, request_id);
  EXPECT_EQ(first_auths[0].username, "alice");

  handle_dbapp_death(old_addr);
  cache_dbapp_channel(new_addr, &new_shard);
  apply_dbapp_shards(
      8, {dbappmgr::ShardEntry{1, std::numeric_limits<DatabaseID>::max(), 9, new_addr}});

  EXPECT_TRUE(drive_until([&] { return new_shard.send_count() > 0; }, Milliseconds(500)));
  const auto retried_auths = DecodeSentMessages<login::AuthLogin>(new_shard);
  ASSERT_EQ(retried_auths.size(), 1u);
  EXPECT_EQ(retried_auths[0].request_id, request_id);
  EXPECT_EQ(retried_auths[0].username, "alice");

  login::AuthLoginResult auth_reply;
  auth_reply.request_id = request_id;
  auth_reply.success = true;
  auth_reply.status = login::LoginStatus::kSuccess;
  auth_reply.dbid = 42;
  auth_reply.type_id = 7;
  handle_auth_result(auth_reply);

  EXPECT_TRUE(pending_by_username_empty());
  EXPECT_EQ(active_login_count(), 0u);
}

class BaseAppRollbackTest : public ::testing::Test {
 protected:
  BaseAppRollbackTest()
      : dispatcher_("baseapp_rollback"),
        internal_network_(dispatcher_),
        external_network_(dispatcher_),
        app_(dispatcher_, internal_network_, external_network_) {
    app_.RegisterInternalHandlers();
  }

  void register_watchers() { app_.RegisterWatchers(); }

  void seed_expired_prepared_login(uint32_t login_request_id, EntityID entity_id) {
    app_.prepared_login_entities_[login_request_id] = BaseApp::PreparedLoginEntity{
        entity_id, 1234, 7, Clock::now() - BaseApp::kPreparedLoginTimeout - std::chrono::seconds(1),
        "alice"};
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
  void set_legacy_dbapp_channel(Channel* ch) { app_.dbapp_channel_ = ch; }
  void cache_dbapp_channel(const Address& addr, Channel* ch) { app_.dbapp_channels_[addr] = ch; }
  void handle_dbapp_death(const Address& addr) {
    machined::DeathNotification death;
    death.internal_addr = addr;
    death.reason = 1;
    app_.OnDbAppDeath(death);
  }
  void apply_dbapp_shards(uint32_t version, std::vector<dbappmgr::ShardEntry> entries) {
    app_.ApplyDbAppShardTable(version, std::move(entries));
  }
  auto resolve_dbapp(DatabaseID dbid) -> Channel* { return app_.ResolveDbAppChannel(dbid); }
  auto dbapp_shard_version() const -> uint32_t { return app_.dbapp_shard_table_version_; }
  auto dbapp_shard_count() const -> std::size_t { return app_.dbapp_shard_table_.size(); }
  auto submit_prepare_login(DatabaseID dbid, uint16_t type_id = 7) -> uint32_t {
    BaseApp::PendingLogin pending;
    pending.login_request_id = 7001;
    pending.loginapp_addr = Address(0x7F000001u, 25001);
    pending.type_id = type_id;
    pending.dbid = dbid;
    pending.created_at = Clock::now();
    const uint32_t request_id = app_.next_prepare_request_id_;
    app_.SubmitPrepareLogin(std::move(pending));
    return request_id;
  }
  auto pending_login_contains(uint32_t request_id) const -> bool {
    return app_.pending_logins_.contains(request_id);
  }
  auto pending_login_retry_pending(uint32_t request_id) const -> bool {
    auto it = app_.pending_logins_.find(request_id);
    return it != app_.pending_logins_.end() && it->second.checkout_retry_pending;
  }
  auto active_login_contains(DatabaseID dbid) const -> bool {
    return app_.active_login_dbids_.contains(dbid);
  }
  auto pending_write_to_db_count() const -> std::size_t { return app_.pending_write_to_db_.size(); }
  auto pending_write_to_db_contains(uint32_t request_id) const -> bool {
    return app_.pending_write_to_db_.contains(request_id);
  }
  auto pending_logoff_write_contains(uint32_t request_id) const -> bool {
    return app_.pending_logoff_writes_.contains(request_id);
  }
  auto pending_logoff_write_retry_pending(uint32_t request_id) const -> bool {
    auto it = app_.pending_logoff_writes_.find(request_id);
    return it != app_.pending_logoff_writes_.end() && it->second.retry_pending;
  }
  auto logoff_in_flight_contains(EntityID entity_id) const -> bool {
    return app_.logoff_entities_in_flight_.contains(entity_id);
  }
  void handle_write_ack(const dbapp::WriteEntityAck& ack) {
    BinaryWriter writer;
    ack.Serialize(writer);
    BinaryReader reader(writer.Data());
    ASSERT_TRUE(internal_network_.InterfaceTable()
                    .Dispatch(Address{}, nullptr, dbapp::WriteEntityAck::Descriptor().id, reader)
                    .HasValue());
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
  auto create_db_entity(DatabaseID dbid, uint16_t type_id = 7) -> BaseEntity* {
    app_.entity_mgr_.SetIdClient(&app_.id_client_);
    if (!ids_seeded_) {
      app_.id_client_.AddIds(1000, 1100);
      ids_seeded_ = true;
    }
    return app_.entity_mgr_.Create(type_id, false, dbid);
  }
  void write_to_db(EntityID entity_id, std::span<const std::byte> data) {
    app_.DoWriteToDb(entity_id, data.data(), static_cast<int32_t>(data.size()));
  }
  void start_disconnect_logoff(EntityID entity_id) { app_.StartDisconnectLogoff(entity_id); }

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
  void seed_death_restore_counters(uint64_t notifications, uint64_t scheduled, uint64_t restored,
                                   uint64_t lost, uint64_t timeouts) {
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
  const auto addr_a_entry =
      "addr=" + addr_a.ToString() + " entities=2 payload_candidates=1 ghost_backup_candidates=1";
  const auto addr_b_entry =
      "addr=" + addr_b.ToString() + " entities=1 payload_candidates=0 ghost_backup_candidates=1";
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
  EXPECT_EQ(watcher("baseapp/cellapp_death_restore_payload_scheduled_total").value_or(""), "0");
  EXPECT_EQ(watcher("baseapp/cellapp_death_restore_ghost_backup_scheduled_total").value_or(""),
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

TEST_F(BaseAppRollbackTest, DbAppShardTableRoutesDbidAndKeepsLegacyFallback) {
  register_watchers();
  InterfaceTable channel_table;
  CapturingChannel legacy(dispatcher_, channel_table, Address(0x7F000001u, 24001));
  CapturingChannel shard(dispatcher_, channel_table, Address(0x7F000001u, 24002));
  const Address shard_addr(0x7F000001u, 24002);

  set_legacy_dbapp_channel(&legacy);
  cache_dbapp_channel(shard_addr, &shard);
  apply_dbapp_shards(5, {dbappmgr::ShardEntry{1, 1000, 8, shard_addr, false}});

  EXPECT_EQ(resolve_dbapp(42), &shard);
  EXPECT_EQ(resolve_dbapp(1000), nullptr);
  EXPECT_EQ(resolve_dbapp(kInvalidDBID), &legacy);
  EXPECT_EQ(dbapp_shard_version(), 5u);
  EXPECT_EQ(dbapp_shard_count(), 1u);
  EXPECT_EQ(watcher("baseapp/dbapp_shard_table_version").value_or(""), "5");
  EXPECT_EQ(watcher("baseapp/dbapp_shard_count").value_or(""), "1");
}

TEST_F(BaseAppRollbackTest, WriteToDbUsesShardChannelForKnownDbid) {
  InterfaceTable channel_table;
  CapturingChannel legacy(dispatcher_, channel_table, Address(0x7F000001u, 24001));
  CapturingChannel shard(dispatcher_, channel_table, Address(0x7F000001u, 24002));
  const Address shard_addr(0x7F000001u, 24002);

  set_legacy_dbapp_channel(&legacy);
  cache_dbapp_channel(shard_addr, &shard);
  apply_dbapp_shards(7, {dbappmgr::ShardEntry{1, 1000, 8, shard_addr, false}});
  auto* ent = create_db_entity(42);
  ASSERT_NE(ent, nullptr);

  const std::array<std::byte, 3> blob{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
  write_to_db(ent->EntityId(), blob);

  EXPECT_EQ(shard.send_count(), 1u);
  EXPECT_EQ(legacy.send_count(), 0u);
  const auto writes = DecodeSentMessages<dbapp::WriteEntity>(shard);
  ASSERT_EQ(writes.size(), 1u);
  EXPECT_EQ(writes[0].shard_table_version, 7u);
  EXPECT_NE(writes[0].request_id, ent->EntityId());
  EXPECT_EQ(pending_write_to_db_count(), 1u);
  EXPECT_TRUE(pending_write_to_db_contains(writes[0].request_id));

  dbapp::WriteEntityAck ack;
  ack.request_id = writes[0].request_id;
  ack.success = true;
  ack.dbid = 42;
  handle_write_ack(ack);
  EXPECT_FALSE(pending_write_to_db_contains(writes[0].request_id));
}

TEST_F(BaseAppRollbackTest, WriteToDbRetriesPendingWriteAfterDbAppDeathAndShardUpdate) {
  InterfaceTable channel_table;
  CapturingChannel old_shard(dispatcher_, channel_table, Address(0x7F000001u, 24002));
  CapturingChannel new_shard(dispatcher_, channel_table, Address(0x7F000001u, 24003));
  const Address old_addr(0x7F000001u, 24002);
  const Address new_addr(0x7F000001u, 24003);

  cache_dbapp_channel(old_addr, &old_shard);
  apply_dbapp_shards(7, {dbappmgr::ShardEntry{1, 1000, 8, old_addr, false}});
  auto* ent = create_db_entity(42);
  ASSERT_NE(ent, nullptr);

  const std::array<std::byte, 3> blob{std::byte{0x04}, std::byte{0x05}, std::byte{0x06}};
  write_to_db(ent->EntityId(), blob);

  const auto first_writes = DecodeSentMessages<dbapp::WriteEntity>(old_shard);
  ASSERT_EQ(first_writes.size(), 1u);
  const uint32_t request_id = first_writes[0].request_id;
  EXPECT_TRUE(pending_write_to_db_contains(request_id));

  handle_dbapp_death(old_addr);
  EXPECT_TRUE(pending_write_to_db_contains(request_id));

  cache_dbapp_channel(new_addr, &new_shard);
  apply_dbapp_shards(8, {dbappmgr::ShardEntry{1, 1000, 9, new_addr, false}});

  const auto retried_writes = DecodeSentMessages<dbapp::WriteEntity>(new_shard);
  ASSERT_EQ(retried_writes.size(), 1u);
  EXPECT_EQ(retried_writes[0].request_id, request_id);
  EXPECT_EQ(retried_writes[0].shard_table_version, 8u);
  EXPECT_EQ(retried_writes[0].dbid, 42);
  EXPECT_EQ(retried_writes[0].blob, std::vector<std::byte>(blob.begin(), blob.end()));

  dbapp::WriteEntityAck ack;
  ack.request_id = request_id;
  ack.success = true;
  ack.dbid = 42;
  handle_write_ack(ack);
  EXPECT_FALSE(pending_write_to_db_contains(request_id));
}

TEST_F(BaseAppRollbackTest, LogoffPersistRetriesPendingWriteAfterDbAppDeathAndShardUpdate) {
  InterfaceTable channel_table;
  CapturingChannel old_shard(dispatcher_, channel_table, Address(0x7F000001u, 24002));
  CapturingChannel new_shard(dispatcher_, channel_table, Address(0x7F000001u, 24003));
  const Address old_addr(0x7F000001u, 24002);
  const Address new_addr(0x7F000001u, 24003);

  cache_dbapp_channel(old_addr, &old_shard);
  apply_dbapp_shards(7, {dbappmgr::ShardEntry{1, 1000, 8, old_addr, false}});
  auto* ent = create_db_entity(42);
  ASSERT_NE(ent, nullptr);
  ent->SetEntityData({std::byte{0x11}, std::byte{0x12}});
  const EntityID entity_id = ent->EntityId();

  start_disconnect_logoff(entity_id);

  const auto first_writes = DecodeSentMessages<dbapp::WriteEntity>(old_shard);
  ASSERT_EQ(first_writes.size(), 1u);
  const uint32_t request_id = first_writes[0].request_id;
  EXPECT_EQ(first_writes[0].entity_id, entity_id);
  EXPECT_EQ(first_writes[0].dbid, 42);
  EXPECT_EQ(first_writes[0].shard_table_version, 7u);
  EXPECT_TRUE(pending_logoff_write_contains(request_id));
  EXPECT_TRUE(logoff_in_flight_contains(entity_id));
  EXPECT_FALSE(ent->IsPendingDestroy());

  handle_dbapp_death(old_addr);
  EXPECT_TRUE(pending_logoff_write_contains(request_id));
  EXPECT_TRUE(pending_logoff_write_retry_pending(request_id));
  EXPECT_TRUE(logoff_in_flight_contains(entity_id));
  EXPECT_FALSE(ent->IsPendingDestroy());

  cache_dbapp_channel(new_addr, &new_shard);
  apply_dbapp_shards(8, {dbappmgr::ShardEntry{1, 1000, 9, new_addr, false}});

  const auto retried_writes = DecodeSentMessages<dbapp::WriteEntity>(new_shard);
  ASSERT_EQ(retried_writes.size(), 1u);
  EXPECT_EQ(retried_writes[0].request_id, request_id);
  EXPECT_EQ(retried_writes[0].entity_id, entity_id);
  EXPECT_EQ(retried_writes[0].dbid, 42);
  EXPECT_EQ(retried_writes[0].shard_table_version, 8u);
  EXPECT_EQ(retried_writes[0].blob, first_writes[0].blob);
  EXPECT_FALSE(pending_logoff_write_retry_pending(request_id));

  dbapp::WriteEntityAck ack;
  ack.request_id = request_id;
  ack.success = true;
  ack.dbid = 42;
  handle_write_ack(ack);

  EXPECT_FALSE(pending_logoff_write_contains(request_id));
  EXPECT_FALSE(logoff_in_flight_contains(entity_id));
  EXPECT_TRUE(ent->IsPendingDestroy());
  EXPECT_EQ(ent->Dbid(), kInvalidDBID);
}

TEST_F(BaseAppRollbackTest, PrepareLoginCheckoutRetriesAfterDbAppDeathAndShardUpdate) {
  InterfaceTable channel_table;
  CapturingChannel old_shard(dispatcher_, channel_table, Address(0x7F000001u, 24002));
  CapturingChannel new_shard(dispatcher_, channel_table, Address(0x7F000001u, 24003));
  const Address old_addr(0x7F000001u, 24002);
  const Address new_addr(0x7F000001u, 24003);

  cache_dbapp_channel(old_addr, &old_shard);
  apply_dbapp_shards(7, {dbappmgr::ShardEntry{1, 1000, 8, old_addr, false}});

  const uint32_t request_id = submit_prepare_login(42);

  const auto first_checkouts = DecodeSentMessages<dbapp::CheckoutEntity>(old_shard);
  ASSERT_EQ(first_checkouts.size(), 1u);
  EXPECT_EQ(first_checkouts[0].request_id, request_id);
  EXPECT_EQ(first_checkouts[0].dbid, 42);
  EXPECT_EQ(first_checkouts[0].type_id, 7);
  EXPECT_EQ(first_checkouts[0].shard_table_version, 7u);
  EXPECT_TRUE(pending_login_contains(request_id));
  EXPECT_TRUE(active_login_contains(42));

  handle_dbapp_death(old_addr);
  EXPECT_TRUE(pending_login_contains(request_id));
  EXPECT_TRUE(pending_login_retry_pending(request_id));
  EXPECT_TRUE(active_login_contains(42));

  cache_dbapp_channel(new_addr, &new_shard);
  apply_dbapp_shards(8, {dbappmgr::ShardEntry{1, 1000, 9, new_addr, false}});

  const auto retried_checkouts = DecodeSentMessages<dbapp::CheckoutEntity>(new_shard);
  ASSERT_EQ(retried_checkouts.size(), 1u);
  EXPECT_EQ(retried_checkouts[0].request_id, request_id);
  EXPECT_EQ(retried_checkouts[0].dbid, 42);
  EXPECT_EQ(retried_checkouts[0].type_id, 7);
  EXPECT_EQ(retried_checkouts[0].shard_table_version, 8u);
  EXPECT_TRUE(pending_login_contains(request_id));
  EXPECT_FALSE(pending_login_retry_pending(request_id));
  EXPECT_TRUE(active_login_contains(42));
}

class FakeDatabase final : public IDatabase {
 public:
  Result<void> Startup(const DatabaseConfig&, const EntityDefRegistry&) override { return {}; }
  void Shutdown() override {}

  void PutEntity(DatabaseID dbid, uint16_t type_id, WriteFlags flags, std::span<const std::byte>,
                 const std::string& identifier, std::function<void(PutResult)> callback) override {
    ++put_entity_calls;
    last_put_dbid = dbid;
    last_put_type_id = type_id;
    last_put_flags = flags;
    last_put_identifier = identifier;
    if (!complete_put_immediately) {
      put_callback = std::move(callback);
      return;
    }
    PutResult result;
    if (duplicate_dbid_failures_before_success > 0) {
      --duplicate_dbid_failures_before_success;
      result.dbid = dbid;
      result.error = "duplicate dbid";
      result.error_kind = DatabaseErrorKind::kDuplicateDbid;
      callback(std::move(result));
      return;
    }
    result.success = put_success;
    result.dbid = dbid != kInvalidDBID ? dbid : fallback_put_dbid;
    result.error = put_error;
    callback(std::move(result));
  }

  void PutEntityWithPassword(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                             std::span<const std::byte>, const std::string& identifier,
                             const std::string& password_hash,
                             std::function<void(PutResult)> callback) override {
    ++put_entity_with_password_calls;
    last_put_dbid = dbid;
    last_put_type_id = type_id;
    last_put_flags = flags;
    last_put_identifier = identifier;
    last_password_hash = password_hash;
    if (!complete_put_immediately) {
      put_callback = std::move(callback);
      return;
    }
    PutResult result;
    if (duplicate_dbid_failures_before_success > 0) {
      --duplicate_dbid_failures_before_success;
      result.dbid = dbid;
      result.error = "duplicate dbid";
      result.error_kind = DatabaseErrorKind::kDuplicateDbid;
      callback(std::move(result));
      return;
    }
    result.success = put_success;
    result.dbid = dbid != kInvalidDBID ? dbid : fallback_put_dbid;
    result.error = put_error;
    callback(std::move(result));
  }

  void GetEntity(DatabaseID, uint16_t, std::function<void(GetResult)>) override {
    ADD_FAILURE() << "GetEntity should not be called in this test";
  }

  void DelEntity(DatabaseID, uint16_t, std::function<void(DelResult)>) override {
    ADD_FAILURE() << "DelEntity should not be called in this test";
  }

  void LookupByName(uint16_t type_id, const std::string& identifier,
                    std::function<void(LookupResult)> callback) override {
    ++lookup_by_name_calls;
    last_lookup_type_id = type_id;
    last_lookup_identifier = identifier;
    LookupResult result;
    result.found = lookup_found;
    result.dbid = lookup_dbid;
    result.password_hash = lookup_password_hash;
    result.error = lookup_error;
    callback(std::move(result));
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
  void GetMaxDbidInRange(DatabaseID low, DatabaseID high,
                         std::function<void(DbidRangeResult)> callback) override {
    max_dbid_range_requests.push_back({low, high});
    if (!complete_range_query_immediately) {
      range_query_callback = std::move(callback);
      return;
    }
    DbidRangeResult result;
    result.success = range_query_success;
    result.max_dbid = range_max_dbid;
    result.error = range_query_error;
    callback(std::move(result));
  }
  void LoadEntityIdCounter(std::function<void(EntityID)> callback) override { callback(1); }
  void SaveEntityIdCounter(EntityID, std::function<void(bool)> callback) override {
    callback(true);
  }
  void ProcessResults() override {}

  std::function<void(GetResult)> checkout_callback;
  int put_entity_calls{0};
  int put_entity_with_password_calls{0};
  DatabaseID last_put_dbid{kInvalidDBID};
  uint16_t last_put_type_id{0};
  WriteFlags last_put_flags{WriteFlags::kNone};
  std::string last_put_identifier;
  std::string last_password_hash;
  bool complete_put_immediately{true};
  std::function<void(PutResult)> put_callback;
  bool put_success{true};
  DatabaseID fallback_put_dbid{42};
  std::string put_error;
  int duplicate_dbid_failures_before_success{0};
  int lookup_by_name_calls{0};
  uint16_t last_lookup_type_id{0};
  std::string last_lookup_identifier;
  bool lookup_found{false};
  DatabaseID lookup_dbid{kInvalidDBID};
  std::string lookup_password_hash;
  std::string lookup_error;
  std::vector<std::pair<DatabaseID, DatabaseID>> max_dbid_range_requests;
  bool complete_range_query_immediately{true};
  bool range_query_success{true};
  DatabaseID range_max_dbid{kInvalidDBID};
  std::string range_query_error;
  std::function<void(DbidRangeResult)> range_query_callback;
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

  void write_entity(Channel* ch, const dbapp::WriteEntity& write) {
    app_.OnWriteEntity(Address("127.0.0.1", 30001), ch, write);
  }

  void auth_login(Channel* ch, const login::AuthLogin& auth) {
    app_.OnAuthLogin(Address("127.0.0.1", 30001), ch, auth);
  }

  void checkout_entity(Channel* ch, const dbapp::CheckoutEntity& checkout) {
    app_.OnCheckoutEntity(Address("127.0.0.1", 30001), ch, checkout);
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
    return app_.pending_checkout_requests_.contains(request_key(request_id));
  }

  auto pending_request_canceled(uint32_t request_id) const -> bool {
    return app_.pending_checkout_requests_.at(request_key(request_id)).canceled;
  }

  auto pending_request_cleared_dbid(uint32_t request_id) const -> DatabaseID {
    return app_.pending_checkout_requests_.at(request_key(request_id)).cleared_dbid;
  }

  auto checkout_owner(DatabaseID dbid, uint16_t type_id) const -> std::optional<CheckoutInfo> {
    return app_.checkout_mgr_.GetOwner(dbid, type_id);
  }

  auto abort_total() const -> uint64_t { return app_.abort_checkout_total_; }
  auto abort_pending_total() const -> uint64_t { return app_.abort_checkout_pending_hit_total_; }
  auto abort_late_total() const -> uint64_t { return app_.abort_checkout_late_hit_total_; }
  auto dbapp_id() const -> uint32_t { return app_.dbapp_id_; }
  auto shard_table_version() const -> uint32_t { return app_.shard_table_version_; }
  void set_shard_table_version(uint32_t version) { app_.shard_table_version_ = version; }
  void set_auto_create_accounts(bool enabled, uint16_t account_type_id) {
    app_.auto_create_accounts_ = enabled;
    app_.account_type_id_ = account_type_id;
  }
  auto shard_count() const -> std::size_t { return app_.shard_table_.size(); }
  auto shard_app_id(std::size_t index) const -> uint32_t {
    return app_.shard_table_.at(index).dbapp_id;
  }
  void handle_register_dbapp_ack(const dbappmgr::RegisterDbAppAck& ack) {
    app_.OnRegisterDbAppAck(Address(0x7F000001u, 28001), nullptr, ack);
  }
  void handle_shard_table_update(const dbappmgr::ShardTableUpdate& update) {
    app_.OnShardTableUpdate(Address(0x7F000001u, 28001), nullptr, update);
  }
  auto watcher(std::string_view path) -> std::optional<std::string> {
    return app_.GetWatcherRegistry().Get(path);
  }

  auto request_key(uint32_t request_id) const -> DBApp::RequestCacheKey {
    return DBApp::RequestCacheKey{Address("127.0.0.1", 30001), request_id};
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

TEST_F(DBAppRollbackTest, RegisterDbAppAckStoresShardState) {
  register_watchers();

  dbappmgr::RegisterDbAppAck ack;
  ack.success = true;
  ack.dbapp_id = 7;
  ack.shard_table_version = 3;
  ack.entries.push_back(dbappmgr::ShardEntry{1, 1000, 7, Address(0x7F000001u, 24001), false});

  handle_register_dbapp_ack(ack);

  EXPECT_EQ(dbapp_id(), 7u);
  EXPECT_EQ(shard_table_version(), 3u);
  EXPECT_EQ(shard_count(), 1u);
  EXPECT_EQ(watcher("dbapp/dbapp_id").value_or(""), "7");
  EXPECT_EQ(watcher("dbapp/shard_table_version").value_or(""), "3");
}

TEST_F(DBAppRollbackTest, RegisterDbAppAckKeepsLocalShardStateForRecoveryAck) {
  dbappmgr::RegisterDbAppAck ack;
  ack.success = true;
  ack.dbapp_id = 7;
  ack.shard_table_version = 3;
  ack.entries.push_back(dbappmgr::ShardEntry{1, 1000, 7, Address(0x7F000001u, 24001), false});
  handle_register_dbapp_ack(ack);

  dbappmgr::RegisterDbAppAck recovering;
  recovering.success = true;
  recovering.dbapp_id = 7;
  handle_register_dbapp_ack(recovering);

  EXPECT_EQ(dbapp_id(), 7u);
  EXPECT_EQ(shard_table_version(), 3u);
  ASSERT_EQ(shard_count(), 1u);
  EXPECT_EQ(shard_app_id(0), 7u);
}

TEST_F(DBAppRollbackTest, ShardTableUpdateReplacesCachedTable) {
  dbappmgr::RegisterDbAppAck ack;
  ack.success = true;
  ack.dbapp_id = 7;
  ack.shard_table_version = 3;
  ack.entries.push_back(dbappmgr::ShardEntry{1, 1000, 7, Address(0x7F000001u, 24001), false});
  handle_register_dbapp_ack(ack);

  dbappmgr::ShardTableUpdate update;
  update.version = 4;
  update.entries.push_back(dbappmgr::ShardEntry{1, 500, 7, Address(0x7F000001u, 24001), false});
  update.entries.push_back(dbappmgr::ShardEntry{500, 1000, 8, Address(0x7F000001u, 24002), false});

  handle_shard_table_update(update);

  EXPECT_EQ(shard_table_version(), 4u);
  ASSERT_EQ(shard_count(), 2u);
  EXPECT_EQ(shard_app_id(1), 8u);
}

TEST_F(DBAppRollbackTest, ShardTableUpdateIgnoresStaleVersion) {
  dbappmgr::RegisterDbAppAck ack;
  ack.success = true;
  ack.dbapp_id = 7;
  ack.shard_table_version = 4;
  ack.entries.push_back(dbappmgr::ShardEntry{1, 1000, 7, Address(0x7F000001u, 24001), false});
  handle_register_dbapp_ack(ack);

  dbappmgr::ShardTableUpdate stale;
  stale.version = 3;
  stale.entries.push_back(dbappmgr::ShardEntry{1, 1000, 8, Address(0x7F000001u, 24002), false});
  handle_shard_table_update(stale);

  EXPECT_EQ(shard_table_version(), 4u);
  ASSERT_EQ(shard_count(), 1u);
  EXPECT_EQ(shard_app_id(0), 7u);
}

TEST_F(DBAppRollbackTest, WriteEntityCreateAllocatesExplicitDbidFromOwnedShard) {
  db().complete_put_immediately = false;
  dbappmgr::RegisterDbAppAck ack;
  ack.success = true;
  ack.dbapp_id = 7;
  ack.shard_table_version = 1;
  ack.entries.push_back(dbappmgr::ShardEntry{1000, 2000, 7, Address(0x7F000001u, 24001), false});
  handle_register_dbapp_ack(ack);

  dbapp::WriteEntity write;
  write.request_id = 77;
  write.type_id = 9;
  write.flags = WriteFlags::kCreateNew;
  write.shard_table_version = 1;
  write_entity(test_support::FakeChannel(0x1505), write);

  EXPECT_EQ(db().put_entity_calls, 1);
  EXPECT_EQ(db().last_put_dbid, 1000);
  EXPECT_TRUE(HasFlag(db().last_put_flags, WriteFlags::kCreateNew));
  EXPECT_TRUE(HasFlag(db().last_put_flags, WriteFlags::kExplicitDbid));
}

TEST_F(DBAppRollbackTest, WriteEntityCreateResumesAfterStoredShardMaxDbid) {
  db().complete_put_immediately = false;
  db().range_max_dbid = 1234;

  dbappmgr::RegisterDbAppAck ack;
  ack.success = true;
  ack.dbapp_id = 7;
  ack.shard_table_version = 1;
  ack.entries.push_back(dbappmgr::ShardEntry{1000, 2000, 7, Address(0x7F000001u, 24001), false});
  handle_register_dbapp_ack(ack);

  ASSERT_EQ(db().max_dbid_range_requests.size(), 1u);
  EXPECT_EQ(db().max_dbid_range_requests[0].first, 1000);
  EXPECT_EQ(db().max_dbid_range_requests[0].second, 2000);

  dbapp::WriteEntity write;
  write.request_id = 78;
  write.type_id = 9;
  write.flags = WriteFlags::kCreateNew;
  write.shard_table_version = 1;
  write_entity(test_support::FakeChannel(0x1505), write);

  EXPECT_EQ(db().put_entity_calls, 1);
  EXPECT_EQ(db().last_put_dbid, 1235);
  EXPECT_TRUE(HasFlag(db().last_put_flags, WriteFlags::kCreateNew));
  EXPECT_TRUE(HasFlag(db().last_put_flags, WriteFlags::kExplicitDbid));
}

TEST_F(DBAppRollbackTest, WriteEntityCreateRetriesDuplicateDbidInsideOwnedShard) {
  db().duplicate_dbid_failures_before_success = 1;

  dbappmgr::RegisterDbAppAck ack;
  ack.success = true;
  ack.dbapp_id = 7;
  ack.shard_table_version = 1;
  ack.entries.push_back(dbappmgr::ShardEntry{1000, 2000, 7, Address(0x7F000001u, 24001), false});
  handle_register_dbapp_ack(ack);

  dbapp::WriteEntity write;
  write.request_id = 79;
  write.type_id = 9;
  write.flags = WriteFlags::kCreateNew;
  write.shard_table_version = 1;
  write_entity(test_support::FakeChannel(0x1505), write);

  EXPECT_EQ(db().put_entity_calls, 2);
  EXPECT_EQ(db().last_put_dbid, 1001);
  EXPECT_TRUE(HasFlag(db().last_put_flags, WriteFlags::kCreateNew));
  EXPECT_TRUE(HasFlag(db().last_put_flags, WriteFlags::kExplicitDbid));
}

TEST_F(DBAppRollbackTest, AuthLoginAutoCreateAllocatesDbidFromUsernameShard) {
  db().complete_put_immediately = false;
  set_auto_create_accounts(true, 1);
  const std::string username = "ranged_user";
  const DatabaseID route_key = DbShardRouteKey(username);
  const DatabaseID low = route_key;
  const DatabaseID high = route_key < std::numeric_limits<DatabaseID>::max() - 1
                              ? route_key + 2
                              : std::numeric_limits<DatabaseID>::max();

  dbappmgr::RegisterDbAppAck ack;
  ack.success = true;
  ack.dbapp_id = 7;
  ack.shard_table_version = 1;
  ack.entries.push_back(dbappmgr::ShardEntry{low, high, 7, Address(0x7F000001u, 24001), false});
  handle_register_dbapp_ack(ack);

  login::AuthLogin auth;
  auth.request_id = 88;
  auth.username = username;
  auth.password_hash = "pw_hash";
  auth.auto_create = true;
  auth_login(test_support::FakeChannel(0x1506), auth);

  EXPECT_EQ(db().lookup_by_name_calls, 1);
  EXPECT_EQ(db().last_lookup_identifier, username);
  EXPECT_EQ(db().put_entity_with_password_calls, 1);
  EXPECT_EQ(db().last_put_dbid, low);
  EXPECT_EQ(db().last_put_identifier, username);
  EXPECT_EQ(db().last_password_hash, "pw_hash");
  EXPECT_TRUE(HasFlag(db().last_put_flags, WriteFlags::kCreateNew));
  EXPECT_TRUE(HasFlag(db().last_put_flags, WriteFlags::kExplicitDbid));
}

TEST_F(DBAppRollbackTest, AuthLoginAutoCreateRetriesDuplicateDbidInsideUsernameShard) {
  db().duplicate_dbid_failures_before_success = 1;
  set_auto_create_accounts(true, 1);
  const std::string username = "duplicate_retry_user";
  const DatabaseID route_key = DbShardRouteKey(username);
  const DatabaseID low = route_key;
  const DatabaseID high = route_key < std::numeric_limits<DatabaseID>::max() - 2
                              ? route_key + 3
                              : std::numeric_limits<DatabaseID>::max();

  dbappmgr::RegisterDbAppAck ack;
  ack.success = true;
  ack.dbapp_id = 7;
  ack.shard_table_version = 1;
  ack.entries.push_back(dbappmgr::ShardEntry{low, high, 7, Address(0x7F000001u, 24001), false});
  handle_register_dbapp_ack(ack);

  login::AuthLogin auth;
  auth.request_id = 89;
  auth.username = username;
  auth.password_hash = "pw_hash";
  auth.auto_create = true;
  auth_login(test_support::FakeChannel(0x1506), auth);

  EXPECT_EQ(db().put_entity_with_password_calls, 2);
  EXPECT_EQ(db().last_put_dbid, low + 1);
  EXPECT_EQ(db().last_put_identifier, username);
  EXPECT_EQ(db().last_password_hash, "pw_hash");
  EXPECT_TRUE(HasFlag(db().last_put_flags, WriteFlags::kCreateNew));
  EXPECT_TRUE(HasFlag(db().last_put_flags, WriteFlags::kExplicitDbid));
}

TEST_F(DBAppRollbackTest, WriteEntityRejectsStaleShardTableVersion) {
  set_shard_table_version(9);
  InterfaceTable channel_table;
  CapturingChannel reply(dispatcher_, channel_table, Address(0x7F000001u, 30001));

  dbapp::WriteEntity msg;
  msg.request_id = 44;
  msg.dbid = 777;
  msg.type_id = 9;
  msg.shard_table_version = 8;
  write_entity(&reply, msg);

  const auto acks = DecodeSentMessages<dbapp::WriteEntityAck>(reply);
  ASSERT_EQ(acks.size(), 1u);
  EXPECT_EQ(acks[0].request_id, 44u);
  EXPECT_FALSE(acks[0].success);
  EXPECT_EQ(acks[0].dbid, 777);
  EXPECT_EQ(acks[0].current_shard_table_version, 9u);
  EXPECT_EQ(acks[0].error, std::string(dbapp::kInvalidShardTableError));
}

TEST_F(DBAppRollbackTest, CheckoutEntityRejectsStaleShardTableVersion) {
  set_shard_table_version(9);
  InterfaceTable channel_table;
  CapturingChannel reply(dispatcher_, channel_table, Address(0x7F000001u, 30001));

  dbapp::CheckoutEntity msg;
  msg.request_id = 45;
  msg.dbid = 778;
  msg.type_id = 9;
  msg.shard_table_version = 8;
  checkout_entity(&reply, msg);

  const auto acks = DecodeSentMessages<dbapp::CheckoutEntityAck>(reply);
  ASSERT_EQ(acks.size(), 1u);
  EXPECT_EQ(acks[0].request_id, 45u);
  EXPECT_EQ(acks[0].status, dbapp::CheckoutStatus::kDbError);
  EXPECT_EQ(acks[0].dbid, 778);
  EXPECT_EQ(acks[0].current_shard_table_version, 9u);
  EXPECT_EQ(acks[0].error, std::string(dbapp::kInvalidShardTableError));
  EXPECT_FALSE(pending_request_contains(45));
  EXPECT_FALSE(checkout_owner(778, 9).has_value());
}

}  // namespace atlas
