#include "foundation/latency_histogram.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace atlas {

namespace {

// Octave 0 covers [0, kSubBuckets); octave k>=1 covers
// [kSubBuckets << (k-1), kSubBuckets << k) with step = 1 << (k-1).
constexpr uint64_t OctaveBase(int octave) {
  return octave == 0 ? uint64_t{0}
                     : static_cast<uint64_t>(LatencyHistogram::kSubBuckets) << (octave - 1);
}
constexpr uint64_t OctaveStep(int octave) {
  return octave <= 1 ? uint64_t{1} : (uint64_t{1} << (octave - 1));
}
constexpr uint64_t OctaveUpperBound(int octave) {
  return static_cast<uint64_t>(LatencyHistogram::kSubBuckets) << octave;
}

}  // namespace

auto LatencyHistogram::BucketIndex(uint64_t micros) -> int {
  if (micros < static_cast<uint64_t>(kSubBuckets)) return static_cast<int>(micros);
  int top = 63 - std::countl_zero(micros);
  int octave = top - 2;
  if (octave >= kOctaves) return kOverflowIndex;
  uint64_t base = OctaveBase(octave);
  uint64_t step = OctaveStep(octave);
  int sub = static_cast<int>((micros - base) / step);
  if (sub >= kSubBuckets) sub = kSubBuckets - 1;
  return octave * kSubBuckets + sub;
}

auto LatencyHistogram::BucketLowerBoundMicros(int idx) -> double {
  if (idx >= kOverflowIndex) return static_cast<double>(OctaveUpperBound(kOctaves - 1));
  int octave = idx / kSubBuckets;
  int sub = idx % kSubBuckets;
  return static_cast<double>(OctaveBase(octave) + static_cast<uint64_t>(sub) * OctaveStep(octave));
}

auto LatencyHistogram::BucketUpperBoundMicros(int idx) -> double {
  if (idx >= kOverflowIndex) return std::numeric_limits<double>::infinity();
  int octave = idx / kSubBuckets;
  int sub = idx % kSubBuckets;
  return static_cast<double>(OctaveBase(octave) +
                             static_cast<uint64_t>(sub + 1) * OctaveStep(octave));
}

void LatencyHistogram::Record(std::chrono::nanoseconds latency) {
  Record(std::chrono::duration_cast<std::chrono::microseconds>(latency));
}

void LatencyHistogram::Record(std::chrono::microseconds latency) {
  uint64_t us = latency.count() < 0 ? 0 : static_cast<uint64_t>(latency.count());
  int idx = BucketIndex(us);
  std::lock_guard lock(mu_);
  ++buckets_[idx];
  ++count_;
  if (us > max_micros_) max_micros_ = us;
}

auto LatencyHistogram::Count() const -> uint64_t {
  std::lock_guard lock(mu_);
  return count_;
}

auto LatencyHistogram::MaxMicros() const -> double {
  std::lock_guard lock(mu_);
  return static_cast<double>(max_micros_);
}

auto LatencyHistogram::QuantileMicros(double q) const -> double {
  std::lock_guard lock(mu_);
  if (count_ == 0) return 0.0;
  q = std::clamp(q, 0.0, 1.0);
  uint64_t target = static_cast<uint64_t>(std::ceil(q * static_cast<double>(count_)));
  if (target == 0) target = 1;
  if (target > count_) target = count_;
  uint64_t cum = 0;
  for (int i = 0; i < kBucketCount; ++i) {
    cum += buckets_[i];
    if (cum >= target) {
      double lo = BucketLowerBoundMicros(i);
      double hi = BucketUpperBoundMicros(i);
      double mid = (i == kOverflowIndex) ? lo : (lo + hi) * 0.5;
      double m = static_cast<double>(max_micros_);
      return std::min(mid, m);
    }
  }
  return static_cast<double>(max_micros_);
}

void LatencyHistogram::Reset() {
  std::lock_guard lock(mu_);
  buckets_.fill(0);
  count_ = 0;
  max_micros_ = 0;
}

}  // namespace atlas
