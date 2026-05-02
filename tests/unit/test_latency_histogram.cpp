#include <chrono>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "foundation/latency_histogram.h"

using namespace atlas;
using std::chrono::microseconds;
using std::chrono::nanoseconds;

TEST(LatencyHistogram, EmptyReturnsZero) {
  LatencyHistogram h;
  EXPECT_EQ(h.Count(), 0u);
  EXPECT_EQ(h.MaxMicros(), 0.0);
  EXPECT_EQ(h.QuantileMicros(0.50), 0.0);
  EXPECT_EQ(h.QuantileMicros(0.99), 0.0);
}

TEST(LatencyHistogram, SinglePoint) {
  LatencyHistogram h;
  h.Record(microseconds(123));
  EXPECT_EQ(h.Count(), 1u);
  EXPECT_EQ(h.MaxMicros(), 123.0);
  EXPECT_GT(h.QuantileMicros(0.50), 0.0);
  EXPECT_LE(h.QuantileMicros(0.50), 123.0);
}

TEST(LatencyHistogram, NegativeClampedToZero) {
  LatencyHistogram h;
  h.Record(microseconds(-5));
  EXPECT_EQ(h.Count(), 1u);
  EXPECT_EQ(h.MaxMicros(), 0.0);
}

TEST(LatencyHistogram, NanosConvertedToMicros) {
  LatencyHistogram h;
  h.Record(nanoseconds(2'500'000));
  EXPECT_EQ(h.Count(), 1u);
  EXPECT_EQ(h.MaxMicros(), 2500.0);
}

TEST(LatencyHistogram, BucketIndexMonotonic) {
  int prev = -1;
  for (uint64_t v : {uint64_t{0}, uint64_t{1}, uint64_t{7}, uint64_t{8}, uint64_t{16},
                     uint64_t{1024}, uint64_t{1'000'000}, uint64_t{30'000'000}}) {
    int idx = LatencyHistogram::BucketIndex(v);
    EXPECT_GE(idx, prev) << "value=" << v;
    prev = idx;
  }
}

TEST(LatencyHistogram, OverflowBucket) {
  int idx = LatencyHistogram::BucketIndex(uint64_t{1} << 40);
  EXPECT_EQ(idx, LatencyHistogram::kOverflowIndex);
}

TEST(LatencyHistogram, BucketBoundsOrdered) {
  for (int i = 0; i < LatencyHistogram::kOverflowIndex; ++i) {
    double lo = LatencyHistogram::BucketLowerBoundMicros(i);
    double hi = LatencyHistogram::BucketUpperBoundMicros(i);
    EXPECT_LT(lo, hi) << "bucket=" << i;
    if (i + 1 < LatencyHistogram::kOverflowIndex) {
      double next_lo = LatencyHistogram::BucketLowerBoundMicros(i + 1);
      EXPECT_LE(hi, next_lo) << "bucket=" << i;
    }
  }
}

TEST(LatencyHistogram, QuantilesOnUniformDistribution) {
  LatencyHistogram h;
  for (int i = 1; i <= 1000; ++i) h.Record(microseconds(i));
  EXPECT_EQ(h.Count(), 1000u);
  EXPECT_EQ(h.MaxMicros(), 1000.0);
  // Resolution within an octave at this range is ~12.5%; allow ±20%.
  double p50 = h.QuantileMicros(0.50);
  double p95 = h.QuantileMicros(0.95);
  double p99 = h.QuantileMicros(0.99);
  EXPECT_NEAR(p50, 500.0, 100.0);
  EXPECT_NEAR(p95, 950.0, 200.0);
  EXPECT_NEAR(p99, 990.0, 200.0);
  EXPECT_LE(p50, p95);
  EXPECT_LE(p95, p99);
  EXPECT_LE(p99, h.MaxMicros());
}

TEST(LatencyHistogram, P100ReturnsAtMostMax) {
  LatencyHistogram h;
  h.Record(microseconds(100));
  h.Record(microseconds(200));
  h.Record(microseconds(50));
  EXPECT_LE(h.QuantileMicros(1.0), h.MaxMicros());
  EXPECT_EQ(h.MaxMicros(), 200.0);
}

TEST(LatencyHistogram, Reset) {
  LatencyHistogram h;
  for (int i = 0; i < 10; ++i) h.Record(microseconds(i * 100));
  h.Reset();
  EXPECT_EQ(h.Count(), 0u);
  EXPECT_EQ(h.MaxMicros(), 0.0);
  EXPECT_EQ(h.QuantileMicros(0.95), 0.0);
}

TEST(LatencyHistogram, ConcurrentRecord) {
  LatencyHistogram h;
  constexpr int kThreads = 8;
  constexpr int kPerThread = 5000;
  std::vector<std::thread> ts;
  for (int t = 0; t < kThreads; ++t) {
    ts.emplace_back([&h, t] {
      for (int i = 0; i < kPerThread; ++i) {
        h.Record(microseconds(((t * 7 + i) % 1000) + 1));
      }
    });
  }
  for (auto& th : ts) th.join();
  EXPECT_EQ(h.Count(), static_cast<uint64_t>(kThreads * kPerThread));
  EXPECT_GT(h.MaxMicros(), 0.0);
  EXPECT_LE(h.MaxMicros(), 1000.0);
}
