#include <vector>

#include <gtest/gtest.h>

#include "server/updatable.h"

using namespace atlas;

// ============================================================================
// Test helper
// ============================================================================

struct CounterUpdatable : Updatable {
  int count = 0;
  void Update() override { ++count; }
};

struct OrderRecorder : Updatable {
  std::vector<int>* order;
  int id;
  OrderRecorder(std::vector<int>* o, int i) : order(o), id(i) {}
  void Update() override { order->push_back(id); }
};

// ============================================================================
// Basic add / remove / call
// ============================================================================

TEST(Updatables, AddAndCall) {
  Updatables updatables;
  CounterUpdatable u;

  EXPECT_TRUE(updatables.Add(&u));
  EXPECT_EQ(updatables.size(), 1u);

  updatables.Call();
  EXPECT_EQ(u.count, 1);

  updatables.Call();
  EXPECT_EQ(u.count, 2);
}

TEST(Updatables, AddReturnsFalseIfAlreadyRegistered) {
  Updatables updatables;
  CounterUpdatable u;

  EXPECT_TRUE(updatables.Add(&u));
  EXPECT_FALSE(updatables.Add(&u));
  EXPECT_EQ(updatables.size(), 1u);
}

TEST(Updatables, RemoveStopsUpdates) {
  Updatables updatables;
  CounterUpdatable u;

  updatables.Add(&u);
  updatables.Call();
  EXPECT_EQ(u.count, 1);

  EXPECT_TRUE(updatables.Remove(&u));
  EXPECT_EQ(updatables.size(), 0u);

  updatables.Call();
  EXPECT_EQ(u.count, 1);  // not called again
}

TEST(Updatables, RemoveReturnsFalseIfNotRegistered) {
  Updatables updatables;
  CounterUpdatable u;
  EXPECT_FALSE(updatables.Remove(&u));
}

// ============================================================================
// Multi-level ordering
// ============================================================================

TEST(Updatables, LevelOrdering) {
  Updatables updatables(3);
  std::vector<int> order;

  OrderRecorder a(&order, 0);  // level 0 — called first
  OrderRecorder b(&order, 1);  // level 1
  OrderRecorder c(&order, 2);  // level 2 — called last

  updatables.Add(&c, 2);
  updatables.Add(&a, 0);
  updatables.Add(&b, 1);

  updatables.Call();

  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], 0);
  EXPECT_EQ(order[1], 1);
  EXPECT_EQ(order[2], 2);
}

TEST(Updatables, MultipleObjectsSameLevel) {
  Updatables updatables(2);
  std::vector<int> order;

  OrderRecorder a(&order, 1);
  OrderRecorder b(&order, 2);
  OrderRecorder c(&order, 3);

  updatables.Add(&a, 0);
  updatables.Add(&b, 0);
  updatables.Add(&c, 1);

  updatables.Call();

  ASSERT_EQ(order.size(), 3u);
  // a and b are at level 0, c at level 1 — c must come after a and b
  EXPECT_EQ(order[2], 3);
  EXPECT_LT(order[0], 3);
  EXPECT_LT(order[1], 3);
}

// ============================================================================
// Safe removal during call()
// ============================================================================

TEST(Updatables, RemoveDuringUpdate) {
  Updatables updatables;

  struct SelfRemover : Updatable {
    Updatables* u;
    int count = 0;
    void Update() override {
      ++count;
      u->Remove(this);
    }
  };

  SelfRemover remover;
  remover.u = &updatables;
  CounterUpdatable other;

  updatables.Add(&remover);
  updatables.Add(&other);

  updatables.Call();

  EXPECT_EQ(remover.count, 1);
  EXPECT_EQ(other.count, 1);
  EXPECT_EQ(updatables.size(), 1u);  // remover removed itself

  updatables.Call();
  EXPECT_EQ(remover.count, 1);  // not called again
  EXPECT_EQ(other.count, 2);
}

TEST(Updatables, RemoveOtherDuringUpdate) {
  Updatables updatables;
  CounterUpdatable victim;

  struct Remover : Updatable {
    Updatables* u;
    Updatable* target;
    void Update() override { u->Remove(target); }
  };

  Remover remover;
  remover.u = &updatables;
  remover.target = &victim;

  // remover at level 0, victim at level 1 — victim hasn't run yet when removed
  Updatables updatables2(2);
  CounterUpdatable victim2;
  Remover remover2;
  remover2.u = &updatables2;
  remover2.target = &victim2;

  updatables2.Add(&remover2, 0);
  updatables2.Add(&victim2, 1);

  updatables2.Call();

  EXPECT_EQ(victim2.count, 0);  // removed before its level ran
  EXPECT_EQ(updatables2.size(), 1u);
}

// ============================================================================
// Add during call() — deferred to next call()
// ============================================================================

TEST(Updatables, AddDuringUpdate) {
  Updatables updatables;
  CounterUpdatable late;

  struct Adder : Updatable {
    Updatables* u;
    Updatable* target;
    void Update() override { u->Add(target); }
  };

  Adder adder;
  adder.u = &updatables;
  adder.target = &late;

  updatables.Add(&adder);

  updatables.Call();
  EXPECT_EQ(late.count, 0);  // not called in the same tick it was added

  updatables.Call();
  EXPECT_EQ(late.count, 1);  // called in the next tick
}

// ============================================================================
// Multiple calls after remove + re-add
// ============================================================================

TEST(Updatables, ReAddAfterRemove) {
  Updatables updatables;
  CounterUpdatable u;

  updatables.Add(&u);
  updatables.Call();
  EXPECT_EQ(u.count, 1);

  updatables.Remove(&u);
  updatables.Call();
  EXPECT_EQ(u.count, 1);

  updatables.Add(&u);
  updatables.Call();
  EXPECT_EQ(u.count, 2);
}

// ============================================================================
// Size tracking
// ============================================================================

TEST(Updatables, SizeTracking) {
  Updatables updatables(3);
  EXPECT_EQ(updatables.size(), 0u);

  CounterUpdatable a, b, c;
  updatables.Add(&a, 0);
  EXPECT_EQ(updatables.size(), 1u);
  updatables.Add(&b, 1);
  EXPECT_EQ(updatables.size(), 2u);
  updatables.Add(&c, 2);
  EXPECT_EQ(updatables.size(), 3u);

  updatables.Remove(&b);
  EXPECT_EQ(updatables.size(), 2u);
}

// ============================================================================
// Empty Updatables
// ============================================================================

TEST(Updatables, CallOnEmptyIsNoOp) {
  Updatables updatables;
  EXPECT_NO_THROW(updatables.Call());
  EXPECT_EQ(updatables.size(), 0u);
}
