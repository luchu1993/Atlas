#ifndef ATLAS_LIB_NETWORK_RTT_ESTIMATOR_H_
#define ATLAS_LIB_NETWORK_RTT_ESTIMATOR_H_

#include <algorithm>

#include "foundation/clock.h"

namespace atlas {

class RttEstimator {
 public:
  void Update(Duration sample) {
    auto s_us = std::chrono::duration_cast<Microseconds>(sample).count();

    if (rtt_us_ == 0) {
      rtt_us_ = s_us;
      var_us_ = s_us / 2;
    } else {
      auto delta = rtt_us_ > s_us ? rtt_us_ - s_us : s_us - rtt_us_;
      var_us_ = (var_us_ * 3 + delta) / 4;
      rtt_us_ = (rtt_us_ * 7 + s_us) / 8;
    }

    auto rto_us = rtt_us_ + var_us_ * 4;
    auto min_us = nodelay_ ? 30'000 : 200'000;
    auto max_us = 5'000'000;
    rto_us_ = std::clamp(rto_us, static_cast<int64_t>(min_us), static_cast<int64_t>(max_us));
  }

  void SetNodelay(bool enable) { nodelay_ = enable; }
  [[nodiscard]] auto Nodelay() const -> bool { return nodelay_; }

  [[nodiscard]] auto Rtt() const -> Duration { return Microseconds(rtt_us_); }
  [[nodiscard]] auto RttVar() const -> Duration { return Microseconds(var_us_); }
  [[nodiscard]] auto Rto() const -> Duration { return Microseconds(rto_us_); }

  [[nodiscard]] auto BackoffRto() const -> Duration {
    int64_t backed = nodelay_ ? rto_us_ * 3 / 2 : rto_us_ * 2;
    return Microseconds(std::min(backed, int64_t{5'000'000}));
  }

 private:
  int64_t rtt_us_{0};
  int64_t var_us_{0};
  int64_t rto_us_{500'000};
  bool nodelay_{false};
};

}  // namespace atlas

#endif  // ATLAS_LIB_NETWORK_RTT_ESTIMATOR_H_
