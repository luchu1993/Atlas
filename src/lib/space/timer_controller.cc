#include "space/timer_controller.h"

namespace atlas {

void TimerController::Update(float dt) {
  accumulated_ += dt;

  // Fire every whole `interval` that fits in the accumulator. A huge dt
  // (e.g. from a tick spike) must still produce the correct number of
  // callbacks for a repeating timer; a one-shot fires at most once.
  while (accumulated_ >= interval_) {
    accumulated_ -= interval_;
    ++fire_count_;
    if (on_fire_) on_fire_(*this);

    if (!repeat_) {
      Finish();
      return;
    }

    // For repeating timers, a Finish() inside on_fire_ (game code that
    // decided to cancel mid-callback) must short-circuit the catch-up loop.
    if (IsFinished()) return;
  }
}

}  // namespace atlas
