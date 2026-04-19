// Controller system unit tests — Phase 10 Step 10.3.
//
// Covers: MoveToPointController, TimerController, Controllers container.
// ProximityController is deferred to Step 10.2 because it needs RangeTrigger.

#include <cmath>
#include <memory>

#include <gtest/gtest.h>

#include "math/vector3.h"
#include "space/controllers.h"
#include "space/entity_motion.h"
#include "space/move_controller.h"
#include "space/timer_controller.h"

namespace atlas {
namespace {

// Minimal IEntityMotion stub for tests. Holds position and direction as
// plain fields; no validation. All movement controllers should be fully
// exercisable against this alone.
class MotionStub : public IEntityMotion {
 public:
  MotionStub(math::Vector3 pos = {0, 0, 0}, math::Vector3 dir = {1, 0, 0}) : pos_(pos), dir_(dir) {}

  [[nodiscard]] auto Position() const -> const math::Vector3& override { return pos_; }
  void SetPosition(const math::Vector3& p) override {
    pos_ = p;
    ++set_pos_count_;
  }
  [[nodiscard]] auto Direction() const -> const math::Vector3& override { return dir_; }
  void SetDirection(const math::Vector3& d) override {
    dir_ = d;
    ++set_dir_count_;
  }

  int set_pos_count_{0};
  int set_dir_count_{0};

 private:
  math::Vector3 pos_;
  math::Vector3 dir_;
};

// ============================================================================
// MoveToPointController
// ============================================================================

TEST(MoveToPoint, AdvancesAlongStraightLine) {
  MotionStub motion({0, 0, 0});
  Controllers ctrls;
  // 10 m/s, destination 10m away along +x: after 1.0s we're half-way,
  // after 2.0s we should have arrived and finished.
  auto id =
      ctrls.Add(std::make_unique<MoveToPointController>(math::Vector3{10, 0, 0}, /*speed=*/10.f,
                                                        /*face_movement=*/false),
                &motion, 0);

  ctrls.Update(0.5f);
  EXPECT_NEAR(motion.Position().x, 5.f, 1e-4f);
  EXPECT_TRUE(ctrls.Contains(id));  // not finished yet

  ctrls.Update(0.5f);
  EXPECT_NEAR(motion.Position().x, 10.f, 1e-4f);
  // After this tick the controller called Finish(); it's reaped by the
  // same Compact() pass, so Contains is false even before next tick.
  EXPECT_FALSE(ctrls.Contains(id));
}

TEST(MoveToPoint, ArrivesEarlyWhenStepExceedsRemaining) {
  MotionStub motion({0, 0, 0});
  Controllers ctrls;
  // Destination 2m away, 100 m/s — first tick should snap + finish.
  auto id =
      ctrls.Add(std::make_unique<MoveToPointController>(math::Vector3{2, 0, 0}, /*speed=*/100.f,
                                                        /*face_movement=*/false),
                &motion, 0);
  ctrls.Update(0.1f);

  EXPECT_NEAR(motion.Position().x, 2.f, 1e-4f);
  EXPECT_FALSE(ctrls.Contains(id));
}

TEST(MoveToPoint, FaceMovementRotatesTowardsDestination) {
  MotionStub motion({0, 0, 0}, {1, 0, 0});  // initially facing +x
  Controllers ctrls;
  // Move to +z: facing should rotate to ~(0,0,1) within one tick.
  ctrls.Add(std::make_unique<MoveToPointController>(math::Vector3{0, 0, 10}, /*speed=*/10.f,
                                                    /*face_movement=*/true),
            &motion, 0);
  ctrls.Update(0.5f);

  // Unit +z (tolerate float noise).
  EXPECT_NEAR(motion.Direction().x, 0.f, 1e-4f);
  EXPECT_NEAR(motion.Direction().z, 1.f, 1e-4f);
  EXPECT_GE(motion.set_dir_count_, 1);
}

TEST(MoveToPoint, FaceMovementSuppressedWhenFlagIsFalse) {
  MotionStub motion({0, 0, 0}, {1, 0, 0});
  Controllers ctrls;
  ctrls.Add(std::make_unique<MoveToPointController>(math::Vector3{0, 0, 10}, /*speed=*/10.f,
                                                    /*face_movement=*/false),
            &motion, 0);
  ctrls.Update(0.5f);
  // Direction unchanged.
  EXPECT_EQ(motion.set_dir_count_, 0);
  EXPECT_NEAR(motion.Direction().x, 1.f, 1e-4f);
}

// ============================================================================
// TimerController
// ============================================================================

TEST(Timer, OneShotFiresOnceThenFinishes) {
  Controllers ctrls;
  int fires = 0;
  auto id = ctrls.Add(std::make_unique<TimerController>(
                          /*interval=*/0.5f, /*repeat=*/false, [&](TimerController&) { ++fires; }),
                      /*motion=*/nullptr, 0);

  // Not yet at interval.
  ctrls.Update(0.3f);
  EXPECT_EQ(fires, 0);
  EXPECT_TRUE(ctrls.Contains(id));

  // Crossing the interval fires exactly once and finishes.
  ctrls.Update(0.5f);
  EXPECT_EQ(fires, 1);
  EXPECT_FALSE(ctrls.Contains(id));

  // Further ticks do nothing (controller is gone).
  ctrls.Update(10.f);
  EXPECT_EQ(fires, 1);
}

TEST(Timer, RepeatingFiresEveryInterval) {
  Controllers ctrls;
  int fires = 0;
  ctrls.Add(std::make_unique<TimerController>(0.5f, true, [&](TimerController&) { ++fires; }),
            nullptr, 0);

  ctrls.Update(0.5f);
  EXPECT_EQ(fires, 1);
  ctrls.Update(0.5f);
  EXPECT_EQ(fires, 2);
  ctrls.Update(0.5f);
  EXPECT_EQ(fires, 3);
}

TEST(Timer, RepeatingCatchesUpOnLargeDelta) {
  // A dt of 2s on a 0.5s-interval repeater should fire four times in a
  // single Update pass — tick spikes must not drop work silently.
  Controllers ctrls;
  int fires = 0;
  ctrls.Add(std::make_unique<TimerController>(0.5f, true, [&](TimerController&) { ++fires; }),
            nullptr, 0);
  ctrls.Update(2.0f);
  EXPECT_EQ(fires, 4);
}

TEST(Timer, OverflowTimeCarriesToNextInterval) {
  // dt = 0.6s on interval 0.5s → 1 fire, 0.1s carried; next 0.4s completes
  // the second fire exactly.
  Controllers ctrls;
  int fires = 0;
  ctrls.Add(std::make_unique<TimerController>(0.5f, true, [&](TimerController&) { ++fires; }),
            nullptr, 0);
  ctrls.Update(0.6f);
  EXPECT_EQ(fires, 1);
  ctrls.Update(0.4f);
  EXPECT_EQ(fires, 2);
}

// ============================================================================
// Controllers container — cancellation semantics
// ============================================================================

TEST(Controllers, CancelOutsideUpdateStopsImmediately) {
  Controllers ctrls;
  bool stopped = false;

  // Plain Controller subclass — TimerController is final so we can't
  // override its Stop() directly; a bare Controller is enough to prove
  // the Cancel→Stop callchain.
  class Observable : public Controller {
   public:
    bool* out_stopped;
    explicit Observable(bool* out) : out_stopped(out) {}
    void Update(float) override {}
    void Stop() override { *out_stopped = true; }
  };

  auto id = ctrls.Add(std::make_unique<Observable>(&stopped), nullptr, 0);
  EXPECT_TRUE(ctrls.Cancel(id));
  EXPECT_TRUE(stopped);
  EXPECT_FALSE(ctrls.Contains(id));
}

TEST(Controllers, CancelDuringUpdateIsDeferred) {
  Controllers ctrls;
  ControllerID self_id = 0;
  int fires = 0;

  // Self-cancelling timer. On fire, it asks Controllers to cancel itself;
  // the cancel must not invalidate the iteration we're in.
  ctrls.Add(std::make_unique<TimerController>(0.5f, true,
                                              [&](TimerController&) {
                                                ++fires;
                                                ctrls.Cancel(self_id);
                                              }),
            nullptr, 0);
  // We don't have the id returned from Add captured cleanly because
  // Add needs the id before the lambda captures it — use a trampoline.
  // Simpler: just cancel by walking all ids after observing a fire.
  self_id = 1;  // matches the id assigned by Add's next_id_ start

  ctrls.Update(0.5f);
  EXPECT_EQ(fires, 1);
  // Deferred cancel executed in Compact() — controller is gone.
  EXPECT_EQ(ctrls.Count(), 0u);
}

TEST(Controllers, FinishedControllerReapedAtTickEnd) {
  Controllers ctrls;
  // One-shot timer finishes inside Update; Compact() must remove it.
  ctrls.Add(std::make_unique<TimerController>(0.1f, false), nullptr, 0);
  EXPECT_EQ(ctrls.Count(), 1u);
  ctrls.Update(0.2f);
  EXPECT_EQ(ctrls.Count(), 0u);
}

TEST(Controllers, StopAllDrainsEverything) {
  Controllers ctrls;
  ctrls.Add(std::make_unique<TimerController>(10.f, true), nullptr, 0);
  ctrls.Add(std::make_unique<TimerController>(10.f, true), nullptr, 0);
  ctrls.Add(std::make_unique<TimerController>(10.f, true), nullptr, 0);
  EXPECT_EQ(ctrls.Count(), 3u);

  ctrls.StopAll();
  EXPECT_EQ(ctrls.Count(), 0u);
}

TEST(Controllers, UserArgPlumbedThrough) {
  Controllers ctrls;
  // Add assigns id AFTER ctor, so we inspect via Motion()/UserArg() helpers.
  // Use a quick sniffer controller.
  class Sniffer : public Controller {
   public:
    int seen_user_arg{-1};
    void Start() override { seen_user_arg = UserArg(); }
    void Update(float) override {}
  };

  auto sniffer = std::make_unique<Sniffer>();
  auto* raw = sniffer.get();
  ctrls.Add(std::move(sniffer), nullptr, /*user_arg=*/7);
  EXPECT_EQ(raw->seen_user_arg, 7);
}

}  // namespace
}  // namespace atlas
