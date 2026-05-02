#ifndef ATLAS_LIB_FOUNDATION_LATENCY_HISTOGRAM_H_
#define ATLAS_LIB_FOUNDATION_LATENCY_HISTOGRAM_H_

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace atlas {

// Log-linear histogram: 25 octaves × 8 sub-buckets = 200 + 1 overflow.
// Range 1 µs .. 32 s, ~12.5% relative resolution between octaves.
class LatencyHistogram {
 public:
  static constexpr int kOctaves = 25;
  static constexpr int kSubBuckets = 8;
  static constexpr int kBucketCount = kOctaves * kSubBuckets + 1;
  static constexpr int kOverflowIndex = kBucketCount - 1;

  void Record(std::chrono::nanoseconds latency);
  void Record(std::chrono::microseconds latency);

  [[nodiscard]] auto Count() const -> uint64_t;
  [[nodiscard]] auto MaxMicros() const -> double;
  // q in [0, 1]; returns microseconds. Returns 0 when Count()==0.
  [[nodiscard]] auto QuantileMicros(double q) const -> double;
  void Reset();

  static auto BucketIndex(uint64_t micros) -> int;
  static auto BucketLowerBoundMicros(int idx) -> double;
  static auto BucketUpperBoundMicros(int idx) -> double;

 private:
  mutable std::mutex mu_;
  std::array<uint64_t, kBucketCount> buckets_{};
  uint64_t count_{0};
  uint64_t max_micros_{0};
};

}  // namespace atlas

#endif  // ATLAS_LIB_FOUNDATION_LATENCY_HISTOGRAM_H_
