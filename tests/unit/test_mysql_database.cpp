#define _CRT_SECURE_NO_WARNINGS  // std::getenv is flagged unsafe by MSVC

#include <mysql.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "db_mysql/mysql_database.h"
#include "entitydef/entity_def_registry.h"

using namespace atlas;

namespace {

auto env_or(const char* key, const char* fallback) -> std::string {
  const char* v = std::getenv(key);
  return v != nullptr ? std::string(v) : std::string(fallback);
}

auto make_blob(std::string_view text) -> std::vector<std::byte> {
  std::vector<std::byte> blob;
  blob.reserve(text.size());
  for (char ch : text) {
    blob.push_back(static_cast<std::byte>(ch));
  }
  return blob;
}

void register_account_entity() {
  EntityDefRegistry::Instance().clear();

  std::vector<std::byte> buf;
  auto write_str = [&](const std::string& s) {
    auto len = static_cast<uint32_t>(s.size());
    if (len < 128) {
      buf.push_back(static_cast<std::byte>(len));
    } else {
      buf.push_back(static_cast<std::byte>((len & 0x7F) | 0x80));
      buf.push_back(static_cast<std::byte>(len >> 7));
    }
    for (char c : s) {
      buf.push_back(static_cast<std::byte>(c));
    }
  };
  auto write_u8 = [&](uint8_t v) { buf.push_back(static_cast<std::byte>(v)); };
  auto write_u16 = [&](uint16_t v) {
    buf.push_back(static_cast<std::byte>(v & 0xFF));
    buf.push_back(static_cast<std::byte>(v >> 8));
  };

  write_str("Account");
  write_u16(1);
  write_u8(0);
  write_u8(1);
  write_u8(2);

  write_str("accountName");
  write_u8(11);
  write_u8(1);
  write_u8(1);
  write_u8(5);
  write_u16(0);
  write_u8(1);  // identifier
  write_u8(0);  // reliable

  write_str("level");
  write_u8(6);
  write_u8(1);
  write_u8(1);
  write_u8(5);
  write_u16(1);
  write_u8(0);  // identifier
  write_u8(0);  // reliable

  write_u8(0);

  ASSERT_TRUE(
      EntityDefRegistry::Instance().RegisterType(buf.data(), static_cast<int32_t>(buf.size())));
}

auto make_config() -> DatabaseConfig {
  DatabaseConfig cfg;
  cfg.type = "mysql";
  cfg.mysql_host = env_or("ATLAS_TEST_MYSQL_HOST", "127.0.0.1");
  cfg.mysql_port = static_cast<uint16_t>(std::stoi(env_or("ATLAS_TEST_MYSQL_PORT", "3306")));
  cfg.mysql_user = env_or("ATLAS_TEST_MYSQL_USER", "root");
  cfg.mysql_password = env_or("ATLAS_TEST_MYSQL_PASSWORD", "");
  cfg.mysql_database = env_or("ATLAS_TEST_MYSQL_DATABASE", "atlas_test");
  return cfg;
}

// Drops the backend tables so each test starts on a fresh schema. Returns false
// when the server is unreachable, which the fixture turns into a skip.
auto reset_schema(const DatabaseConfig& cfg) -> bool {
  MYSQL* admin = mysql_init(nullptr);
  if (admin == nullptr) return false;
  unsigned int timeout = 5;
  mysql_options(admin, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
  if (mysql_real_connect(admin, cfg.mysql_host.c_str(), cfg.mysql_user.c_str(),
                         cfg.mysql_password.c_str(), cfg.mysql_database.c_str(), cfg.mysql_port,
                         nullptr, 0) == nullptr) {
    mysql_close(admin);
    return false;
  }
  static constexpr const char* kDrop =
      "DROP TABLE IF EXISTS entities, meta, atlas_entity_id_counter";
  const bool ok =
      mysql_real_query(admin, kDrop, static_cast<unsigned long>(std::strlen(kDrop))) == 0;
  mysql_close(admin);
  return ok;
}

}  // namespace

class MysqlDatabaseTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (std::getenv("ATLAS_TEST_MYSQL") == nullptr) {
      GTEST_SKIP() << "ATLAS_TEST_MYSQL not set; skipping MySQL backend tests";
    }
    register_account_entity();

    cfg_ = make_config();
    if (!reset_schema(cfg_)) {
      GTEST_SKIP() << "cannot reach MySQL at " << cfg_.mysql_host << ":" << cfg_.mysql_port;
    }

    auto start = db_.Startup(cfg_, EntityDefRegistry::Instance());
    ASSERT_TRUE(start.HasValue()) << "db startup failed: " << start.Error().Message();
  }

  void TearDown() override {
    db_.Shutdown();
    EntityDefRegistry::Instance().clear();
  }

  // Deferred-mode results arrive off the worker pool, so pump ProcessResults
  // until the callback fires (or the timeout trips).
  void pump_until(const std::function<bool()>& done, int timeout_ms = 5000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
      db_.ProcessResults();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  MysqlDatabase db_;
  DatabaseConfig cfg_;
};

TEST_F(MysqlDatabaseTest, PutNewEntityAndGet) {
  PutResult put;
  db_.PutEntity(kInvalidDBID, 1, WriteFlags::kCreateNew, make_blob("hello"), "alice",
                [&](PutResult r) { put = std::move(r); });

  ASSERT_TRUE(put.success) << put.error;
  EXPECT_GT(put.dbid, 0);

  GetResult get;
  db_.GetEntity(put.dbid, 1, [&](GetResult r) { get = std::move(r); });
  ASSERT_TRUE(get.success);
  EXPECT_EQ(get.data.blob, make_blob("hello"));
  EXPECT_EQ(get.data.identifier, "alice");
}

TEST_F(MysqlDatabaseTest, PutNewEntityWithExplicitDbidAndGet) {
  constexpr DatabaseID kDbid = 5000;

  PutResult put;
  db_.PutEntity(kDbid, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid, make_blob("explicit"),
                "alice", [&](PutResult r) { put = std::move(r); });

  ASSERT_TRUE(put.success) << put.error;
  EXPECT_EQ(put.dbid, kDbid);

  GetResult get;
  db_.GetEntity(kDbid, 1, [&](GetResult r) { get = std::move(r); });
  ASSERT_TRUE(get.success);
  EXPECT_EQ(get.data.blob, make_blob("explicit"));

  PutResult duplicate;
  db_.PutEntity(kDbid, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid,
                make_blob("duplicate"), "alice_duplicate",
                [&](PutResult r) { duplicate = std::move(r); });
  EXPECT_FALSE(duplicate.success);
  EXPECT_EQ(duplicate.error_kind, DatabaseErrorKind::kDuplicateDbid);

  PutResult auto_put;
  db_.PutEntity(kInvalidDBID, 1, WriteFlags::kCreateNew, make_blob("auto"), "bob",
                [&](PutResult r) { auto_put = std::move(r); });
  ASSERT_TRUE(auto_put.success) << auto_put.error;
  EXPECT_GT(auto_put.dbid, kDbid);
}

TEST_F(MysqlDatabaseTest, GetMaxDbidInRangeReturnsExistingMaximum) {
  PutResult low;
  db_.PutEntity(1500, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid, make_blob("low"),
                "low", [&](PutResult r) { low = std::move(r); });
  ASSERT_TRUE(low.success) << low.error;

  PutResult high;
  db_.PutEntity(1750, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid, make_blob("high"),
                "high", [&](PutResult r) { high = std::move(r); });
  ASSERT_TRUE(high.success) << high.error;

  PutResult outside;
  db_.PutEntity(2500, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid, make_blob("outside"),
                "outside", [&](PutResult r) { outside = std::move(r); });
  ASSERT_TRUE(outside.success) << outside.error;

  DbidRangeResult range;
  db_.GetMaxDbidInRange(1000, 2000, [&](DbidRangeResult r) { range = std::move(r); });
  ASSERT_TRUE(range.success) << range.error;
  EXPECT_EQ(range.max_dbid, 1750);

  DbidRangeResult empty;
  db_.GetMaxDbidInRange(3000, 4000, [&](DbidRangeResult r) { empty = std::move(r); });
  ASSERT_TRUE(empty.success) << empty.error;
  EXPECT_EQ(empty.max_dbid, kInvalidDBID);
}

TEST_F(MysqlDatabaseTest, CheckoutAndClear) {
  PutResult put;
  db_.PutEntity(kInvalidDBID, 1, WriteFlags::kCreateNew, make_blob("data"), "bob",
                [&](PutResult r) { put = std::move(r); });
  ASSERT_TRUE(put.success) << put.error;

  CheckoutInfo owner;
  owner.base_addr = Address(0x7F000001, 7100);
  owner.app_id = 10;
  owner.entity_id = 20;

  GetResult checkout;
  db_.CheckoutEntity(put.dbid, 1, owner, [&](GetResult r) { checkout = std::move(r); });
  ASSERT_TRUE(checkout.success);
  EXPECT_FALSE(checkout.checked_out_by.has_value());

  GetResult second;
  db_.CheckoutEntity(put.dbid, 1, owner, [&](GetResult r) { second = std::move(r); });
  ASSERT_TRUE(second.success);
  ASSERT_TRUE(second.checked_out_by.has_value());
  EXPECT_EQ(second.checked_out_by->app_id, 10u);

  bool cleared = false;
  db_.ClearCheckout(put.dbid, 1, [&](bool ok) { cleared = ok; });
  EXPECT_TRUE(cleared);
}

TEST_F(MysqlDatabaseTest, AutoLoadAndDeferredCallbacks) {
  db_.SetDeferredMode(true);

  PutResult put;
  bool put_done = false;
  db_.PutEntity(kInvalidDBID, 1, WriteFlags::kCreateNew | WriteFlags::kAutoLoadOn,
                make_blob("auto"), "carol", [&](PutResult r) {
                  put = std::move(r);
                  put_done = true;
                });

  EXPECT_FALSE(put.success);  // delivered only through ProcessResults
  pump_until([&] { return put_done; });
  ASSERT_TRUE(put.success) << put.error;

  std::vector<EntityData> entities;
  bool load_done = false;
  db_.GetAutoLoadEntities([&](std::vector<EntityData> rows) {
    entities = std::move(rows);
    load_done = true;
  });
  EXPECT_TRUE(entities.empty());
  pump_until([&] { return load_done; });
  ASSERT_EQ(entities.size(), 1u);
  EXPECT_EQ(entities[0].identifier, "carol");
}

TEST_F(MysqlDatabaseTest, LookupByNameReturnsStoredPasswordHash) {
  PutResult put;
  db_.PutEntityWithPassword(kInvalidDBID, 1, WriteFlags::kCreateNew, make_blob("acct"), "diana",
                            "pw_hash_123", [&](PutResult r) { put = std::move(r); });
  ASSERT_TRUE(put.success) << put.error;

  LookupResult lookup;
  db_.LookupByName(1, "diana", [&](LookupResult r) { lookup = std::move(r); });
  ASSERT_TRUE(lookup.found);
  EXPECT_EQ(lookup.dbid, put.dbid);
  EXPECT_EQ(lookup.password_hash, "pw_hash_123");
}

TEST_F(MysqlDatabaseTest, PutEntityWithPasswordUsesExplicitDbid) {
  constexpr DatabaseID kDbid = 6000;

  PutResult put;
  db_.PutEntityWithPassword(kDbid, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid,
                            make_blob("acct"), "eve", "pw_hash_456",
                            [&](PutResult r) { put = std::move(r); });

  ASSERT_TRUE(put.success) << put.error;
  EXPECT_EQ(put.dbid, kDbid);

  LookupResult lookup;
  db_.LookupByName(1, "eve", [&](LookupResult r) { lookup = std::move(r); });
  ASSERT_TRUE(lookup.found);
  EXPECT_EQ(lookup.dbid, kDbid);
  EXPECT_EQ(lookup.password_hash, "pw_hash_456");
}

TEST_F(MysqlDatabaseTest, CheckoutByNameAndClearCheckoutsForAddress) {
  PutResult alice;
  PutResult bob;
  PutResult chris;
  db_.PutEntity(kInvalidDBID, 1, WriteFlags::kCreateNew, make_blob("a"), "alice",
                [&](PutResult r) { alice = std::move(r); });
  db_.PutEntity(kInvalidDBID, 1, WriteFlags::kCreateNew, make_blob("b"), "bob",
                [&](PutResult r) { bob = std::move(r); });
  db_.PutEntity(kInvalidDBID, 1, WriteFlags::kCreateNew, make_blob("c"), "chris",
                [&](PutResult r) { chris = std::move(r); });
  ASSERT_TRUE(alice.success);
  ASSERT_TRUE(bob.success);
  ASSERT_TRUE(chris.success);

  CheckoutInfo owner_a;
  owner_a.base_addr = Address(0x7F000001, 7201);
  owner_a.app_id = 21;
  owner_a.entity_id = 101;

  CheckoutInfo owner_b;
  owner_b.base_addr = Address(0x7F000001, 7202);
  owner_b.app_id = 22;
  owner_b.entity_id = 102;

  GetResult checkout_alice;
  db_.CheckoutEntityByName(1, "alice", owner_a,
                           [&](GetResult r) { checkout_alice = std::move(r); });
  ASSERT_TRUE(checkout_alice.success);
  EXPECT_FALSE(checkout_alice.checked_out_by.has_value());

  GetResult checkout_bob;
  db_.CheckoutEntity(bob.dbid, 1, owner_a, [&](GetResult r) { checkout_bob = std::move(r); });
  ASSERT_TRUE(checkout_bob.success);
  EXPECT_FALSE(checkout_bob.checked_out_by.has_value());

  GetResult checkout_chris;
  db_.CheckoutEntity(chris.dbid, 1, owner_b, [&](GetResult r) { checkout_chris = std::move(r); });
  ASSERT_TRUE(checkout_chris.success);
  EXPECT_FALSE(checkout_chris.checked_out_by.has_value());

  int cleared = 0;
  db_.ClearCheckoutsForAddress(owner_a.base_addr, [&](int count) { cleared = count; });
  EXPECT_EQ(cleared, 2);

  GetResult alice_after_clear;
  db_.GetEntity(alice.dbid, 1, [&](GetResult r) { alice_after_clear = std::move(r); });
  ASSERT_TRUE(alice_after_clear.success);
  EXPECT_FALSE(alice_after_clear.checked_out_by.has_value());

  GetResult chris_after_clear;
  db_.GetEntity(chris.dbid, 1, [&](GetResult r) { chris_after_clear = std::move(r); });
  ASSERT_TRUE(chris_after_clear.success);
  ASSERT_TRUE(chris_after_clear.checked_out_by.has_value());
  EXPECT_EQ(chris_after_clear.checked_out_by->app_id, owner_b.app_id);
}

TEST_F(MysqlDatabaseTest, PutEntityWithPasswordPreservesAutoLoadAndClearsCheckoutOnLogoff) {
  PutResult put;
  db_.PutEntityWithPassword(kInvalidDBID, 1, WriteFlags::kCreateNew | WriteFlags::kAutoLoadOn,
                            make_blob("acct"), "eve", "pw_hash_1",
                            [&](PutResult r) { put = std::move(r); });
  ASSERT_TRUE(put.success) << put.error;

  CheckoutInfo owner;
  owner.base_addr = Address(0x7F000001, 7301);
  owner.app_id = 31;
  owner.entity_id = 301;

  GetResult checkout;
  db_.CheckoutEntity(put.dbid, 1, owner, [&](GetResult r) { checkout = std::move(r); });
  ASSERT_TRUE(checkout.success);
  EXPECT_FALSE(checkout.checked_out_by.has_value());

  PutResult update;
  db_.PutEntityWithPassword(put.dbid, 1, WriteFlags::kLogOff, make_blob("acct_v2"), "", "pw_hash_2",
                            [&](PutResult r) { update = std::move(r); });
  ASSERT_TRUE(update.success) << update.error;
  EXPECT_EQ(update.dbid, put.dbid);

  GetResult get;
  db_.GetEntity(put.dbid, 1, [&](GetResult r) { get = std::move(r); });
  ASSERT_TRUE(get.success);
  EXPECT_EQ(get.data.blob, make_blob("acct_v2"));
  EXPECT_EQ(get.data.identifier, "eve");
  EXPECT_FALSE(get.checked_out_by.has_value());

  LookupResult lookup;
  db_.LookupByName(1, "eve", [&](LookupResult r) { lookup = std::move(r); });
  ASSERT_TRUE(lookup.found);
  EXPECT_EQ(lookup.dbid, put.dbid);
  EXPECT_EQ(lookup.password_hash, "pw_hash_2");

  std::vector<EntityData> auto_load_entities;
  db_.GetAutoLoadEntities(
      [&](std::vector<EntityData> rows) { auto_load_entities = std::move(rows); });
  ASSERT_EQ(auto_load_entities.size(), 1u);
  EXPECT_EQ(auto_load_entities[0].dbid, put.dbid);
}
