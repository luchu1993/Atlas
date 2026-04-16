#include <gtest/gtest.h>

#include "foundation/intrusive_ptr.h"

using namespace atlas;

namespace {

class TestObj : public RefCounted {
 public:
  static int live_count;
  int value;

  explicit TestObj(int v = 0) : value(v) { ++live_count; }
  ~TestObj() override { --live_count; }
};

int TestObj::live_count = 0;

class DerivedObj : public TestObj {
 public:
  explicit DerivedObj(int v) : TestObj(v) {}
};

class IntrusivePtrTest : public ::testing::Test {
 protected:
  void SetUp() override { TestObj::live_count = 0; }
  void TearDown() override { EXPECT_EQ(TestObj::live_count, 0); }
};

}  // namespace

TEST_F(IntrusivePtrTest, ConstructionIncrementsRefCount) {
  auto* raw = new TestObj(1);
  raw->add_ref();  // manual ref to keep alive for inspection
  {
    IntrusivePtr<TestObj> p(raw);
    EXPECT_EQ(raw->ref_count(), 2u);
  }
  EXPECT_EQ(raw->ref_count(), 1u);
  raw->release();  // release manual ref
}

TEST_F(IntrusivePtrTest, CopyIncrementsDestructionDecrements) {
  auto p1 = make_intrusive<TestObj>(10);
  EXPECT_EQ(p1->ref_count(), 1u);

  {
    auto p2 = p1;  // copy
    EXPECT_EQ(p1->ref_count(), 2u);
  }
  EXPECT_EQ(p1->ref_count(), 1u);
}

TEST_F(IntrusivePtrTest, MoveDoesNotChangeRefCount) {
  auto p1 = make_intrusive<TestObj>(20);
  EXPECT_EQ(p1->ref_count(), 1u);

  auto p2 = std::move(p1);
  EXPECT_EQ(p1.get(), nullptr);
  EXPECT_EQ(p2->ref_count(), 1u);
}

TEST_F(IntrusivePtrTest, ObjectDeletedWhenLastPtrDestroyed) {
  {
    auto p = make_intrusive<TestObj>(30);
    EXPECT_EQ(TestObj::live_count, 1);
  }
  EXPECT_EQ(TestObj::live_count, 0);
}

TEST_F(IntrusivePtrTest, AdoptRefDoesNotIncrement) {
  auto* raw = new TestObj(40);
  raw->add_ref();  // simulate existing ref
  EXPECT_EQ(raw->ref_count(), 1u);

  IntrusivePtr<TestObj> p(raw, adopt_ref);
  EXPECT_EQ(p->ref_count(), 1u);  // not incremented
}

TEST_F(IntrusivePtrTest, MakeIntrusiveRefCountOne) {
  auto p = make_intrusive<TestObj>(50);
  EXPECT_EQ(p->ref_count(), 1u);
}

TEST_F(IntrusivePtrTest, DetachReleasesOwnership) {
  auto p = make_intrusive<TestObj>(60);
  auto* raw = p.detach();
  EXPECT_EQ(p.get(), nullptr);
  EXPECT_EQ(raw->ref_count(), 1u);
  raw->release();  // manual cleanup
}

TEST_F(IntrusivePtrTest, NullptrComparison) {
  IntrusivePtr<TestObj> p;
  EXPECT_TRUE(p == nullptr);
  EXPECT_FALSE(p != nullptr);

  p = make_intrusive<TestObj>(70);
  EXPECT_FALSE(p == nullptr);
  EXPECT_TRUE(p != nullptr);
}

TEST_F(IntrusivePtrTest, UpcastFromDerived) {
  auto derived = make_intrusive<DerivedObj>(80);
  IntrusivePtr<TestObj> base = derived;  // implicit upcast
  EXPECT_EQ(base->value, 80);
  EXPECT_EQ(base->ref_count(), 2u);
}

// ============================================================================
// Review issue: AtomicRefCounted thread safety
// ============================================================================

namespace {

class AtomicObj : public AtomicRefCounted {
 public:
  static std::atomic<int> live_count;
  AtomicObj() { ++live_count; }
  ~AtomicObj() override { --live_count; }
};

std::atomic<int> AtomicObj::live_count{0};

}  // namespace

TEST(AtomicIntrusivePtr, ThreadSafety) {
  AtomicObj::live_count = 0;
  {
    auto p = IntrusivePtr<AtomicObj>(new AtomicObj());

    std::vector<std::thread> threads;
    for (int i = 0; i < 10; ++i) {
      threads.emplace_back([p]() mutable {
        // Each thread copies and destroys the pointer many times
        for (int j = 0; j < 1000; ++j) {
          auto copy = p;
          (void)copy->ref_count();
        }
      });
    }
    for (auto& t : threads) {
      t.join();
    }

    EXPECT_EQ(p->ref_count(), 1u);
    EXPECT_EQ(AtomicObj::live_count.load(), 1);
  }
  EXPECT_EQ(AtomicObj::live_count.load(), 0);
}

// ============================================================================
// Review issue: RefCounted copy semantics
// ============================================================================

TEST(RefCounted, CopyDoesNotCopyRefCount) {
  // Test that copying a RefCounted object does NOT copy the ref count
  // Use a minimal class to avoid the TestObj live_count tracking issue
  struct Obj : RefCounted {
    int val;
    explicit Obj(int v) : val(v) {}
  };

  auto p = make_intrusive<Obj>(90);
  EXPECT_EQ(p->ref_count(), 1u);

  // Copy the object itself (not the pointer)
  Obj copy(*p);
  // Copied object should start with ref_count 0
  EXPECT_EQ(copy.ref_count(), 0u);
  EXPECT_EQ(copy.val, 90);
  // Original unchanged
  EXPECT_EQ(p->ref_count(), 1u);
}

// ============================================================================
// Review issue: swap and reset
// ============================================================================

TEST_F(IntrusivePtrTest, SwapExchangesPointers) {
  auto p1 = make_intrusive<TestObj>(1);
  auto p2 = make_intrusive<TestObj>(2);

  swap(p1, p2);  // free function swap

  EXPECT_EQ(p1->value, 2);
  EXPECT_EQ(p2->value, 1);
}

TEST_F(IntrusivePtrTest, ResetReleasesOldAndAcquiresNew) {
  auto p = make_intrusive<TestObj>(1);
  EXPECT_EQ(TestObj::live_count, 1);

  auto* raw = new TestObj(2);
  p.reset(raw);
  EXPECT_EQ(TestObj::live_count, 1);  // old deleted, new acquired
  EXPECT_EQ(p->value, 2);
}
