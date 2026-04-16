#include <functional>
#include <vector>

#include <gtest/gtest.h>

#include "db/idatabase.h"
#include "entity_id_allocator.h"
#include "server/entity_types.h"

using namespace atlas;

// ============================================================================
// Minimal in-memory IDatabase stub for EntityIdAllocator tests.
// Only load_entity_id_counter / save_entity_id_counter are implemented;
// all other pure virtuals are stubbed to satisfy the linker.
// ============================================================================

namespace {

class StubDatabase : public IDatabase {
 public:
  // ---- EntityID counter state (the only part under test) -----------------
  EntityID stored_next_id{0};  // 0 = "no counter stored yet"
  bool save_should_fail{false};

  void LoadEntityIdCounter(std::function<void(EntityID)> callback) override {
    EntityID id = (stored_next_id > 0) ? stored_next_id : 1;
    callback(id);
  }

  void SaveEntityIdCounter(EntityID next_id, std::function<void(bool)> callback) override {
    if (save_should_fail) {
      callback(false);
      return;
    }
    stored_next_id = next_id;
    callback(true);
  }

  // ---- Stubs for the rest of the interface (unused by allocator) ---------
  auto Startup(const DatabaseConfig& /*config*/, const EntityDefRegistry& /*defs*/)
      -> Result<void> override {
    return {};
  }
  void Shutdown() override {}
  void PutEntity(DatabaseID, uint16_t, WriteFlags, std::span<const std::byte>, const std::string&,
                 std::function<void(PutResult)>) override {}
  void GetEntity(DatabaseID, uint16_t, std::function<void(GetResult)>) override {}
  void DelEntity(DatabaseID, uint16_t, std::function<void(DelResult)>) override {}
  void LookupByName(uint16_t, const std::string&, std::function<void(LookupResult)>) override {}
  void CheckoutEntity(DatabaseID, uint16_t, const CheckoutInfo&,
                      std::function<void(GetResult)>) override {}
  void CheckoutEntityByName(uint16_t, const std::string&, const CheckoutInfo&,
                            std::function<void(GetResult)>) override {}
  void ClearCheckout(DatabaseID, uint16_t, std::function<void(bool)>) override {}
  void ClearCheckoutsForAddress(const Address&, std::function<void(int)>) override {}
  void GetAutoLoadEntities(std::function<void(std::vector<EntityData>)>) override {}
  void SetAutoLoad(DatabaseID, uint16_t, bool) override {}
  void ProcessResults() override {}
};

}  // namespace

// ============================================================================
// EntityIdAllocator tests
// ============================================================================

TEST(EntityIdAllocator, StartupLoadsAndPersistsWithBuffer) {
  StubDatabase db;
  db.stored_next_id = 0;  // first boot

  EntityIdAllocator alloc(db);
  bool started = false;
  alloc.Startup([&](bool ok) {
    started = true;
    EXPECT_TRUE(ok);
  });
  ASSERT_TRUE(started);

  // After startup, the allocator should have persisted next_id + buffer
  EXPECT_EQ(alloc.next_id(), 1u);
  // stored value = 1 + 100'000 = 100'001
  EXPECT_EQ(db.stored_next_id, 100'001u);
}

TEST(EntityIdAllocator, AllocateReturnsMonotonicRanges) {
  StubDatabase db;
  EntityIdAllocator alloc(db);
  alloc.Startup([](bool) {});

  auto [s1, e1] = alloc.Allocate(10);
  EXPECT_EQ(s1, 1u);
  EXPECT_EQ(e1, 10u);

  auto [s2, e2] = alloc.Allocate(5);
  EXPECT_EQ(s2, 11u);
  EXPECT_EQ(e2, 15u);

  auto [s3, e3] = alloc.Allocate(1);
  EXPECT_EQ(s3, 16u);
  EXPECT_EQ(e3, 16u);
}

TEST(EntityIdAllocator, AllocateZeroReturnsInvalid) {
  StubDatabase db;
  EntityIdAllocator alloc(db);
  alloc.Startup([](bool) {});

  auto [s, e] = alloc.Allocate(0);
  EXPECT_EQ(s, kInvalidEntityID);
  EXPECT_EQ(e, kInvalidEntityID);

  // next_id unchanged
  EXPECT_EQ(alloc.next_id(), 1u);
}

TEST(EntityIdAllocator, PersistIfNeededTriggersWhenPastBuffer) {
  StubDatabase db;
  EntityIdAllocator alloc(db);
  alloc.Startup([](bool) {});

  // After startup, persisted_up_to = 100'001.
  // Allocate enough IDs to push next_id past the buffer boundary.
  // next_id + 100'000 > persisted_up_to  =>  next_id > 1
  // Actually, at startup: persisted = 1+100'000 = 100'001
  // persist triggers when next_id + 100'000 > persisted_up_to
  // => next_id > 100'001 - 100'000 = 1, so any allocation triggers.

  (void)alloc.Allocate(1);  // next_id = 2

  bool called = false;
  alloc.PersistIfNeeded([&](bool ok) {
    called = true;
    EXPECT_TRUE(ok);
  });
  ASSERT_TRUE(called);
  // Should have persisted a new value
  EXPECT_EQ(db.stored_next_id, alloc.next_id() + 100'000);
}

TEST(EntityIdAllocator, PersistIfNeededSkipsWhenNotNeeded) {
  StubDatabase db;
  EntityIdAllocator alloc(db);
  alloc.Startup([](bool) {});

  // Right after startup, next_id=1, persisted=100'001
  // next_id + buffer = 1 + 100'000 = 100'001, NOT > 100'001
  // So persist_if_needed should be a no-op.
  EntityID before = db.stored_next_id;
  bool called = false;
  alloc.PersistIfNeeded([&](bool ok) {
    called = true;
    EXPECT_TRUE(ok);
  });
  ASSERT_TRUE(called);
  EXPECT_EQ(db.stored_next_id, before);
}

TEST(EntityIdAllocator, CrashRecoverySkipsAllocatedIds) {
  // Simulate: allocator persisted up_to 200'001, then we allocated
  // IDs 1..500 but crashed before the next persist.
  // On recovery, the allocator loads 200'001 and starts from there.
  StubDatabase db;
  db.stored_next_id = 200'001;

  EntityIdAllocator alloc(db);
  alloc.Startup([](bool ok) { EXPECT_TRUE(ok); });

  EXPECT_EQ(alloc.next_id(), 200'001u);

  auto [s, e] = alloc.Allocate(1);
  EXPECT_EQ(s, 200'001u);
  EXPECT_EQ(e, 200'001u);
}

TEST(EntityIdAllocator, ForcePersistAlwaysWrites) {
  StubDatabase db;
  EntityIdAllocator alloc(db);
  alloc.Startup([](bool) {});

  bool called = false;
  alloc.Persist([&](bool ok) {
    called = true;
    EXPECT_TRUE(ok);
  });
  ASSERT_TRUE(called);
  // Force persist always writes, even if not strictly needed
  EXPECT_EQ(db.stored_next_id, alloc.next_id() + 100'000);
}

TEST(EntityIdAllocator, SaveFailureReportedViaCallback) {
  StubDatabase db;
  EntityIdAllocator alloc(db);
  alloc.Startup([](bool) {});

  db.save_should_fail = true;

  bool called = false;
  alloc.Persist([&](bool ok) {
    called = true;
    EXPECT_FALSE(ok);
  });
  ASSERT_TRUE(called);
}

TEST(EntityIdAllocator, LargeAllocationIsContiguous) {
  StubDatabase db;
  EntityIdAllocator alloc(db);
  alloc.Startup([](bool) {});

  auto [s, e] = alloc.Allocate(10'000);
  EXPECT_EQ(s, 1u);
  EXPECT_EQ(e, 10'000u);
  EXPECT_EQ(alloc.next_id(), 10'001u);
}
