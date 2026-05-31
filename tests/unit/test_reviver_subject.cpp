#include "server/reviver_subject.h"

#include <gtest/gtest.h>

#include "network/address.h"

namespace atlas {
namespace {

auto Addr(uint16_t port) -> Address { return Address(0x7F000001u, port); }

TEST(ReviverSubject, SolePingerIsActive) {
  ReviverSubject subject;
  const auto now = TimePoint{} + std::chrono::seconds(1);
  EXPECT_TRUE(subject.RecordPingAndIsActive(Addr(1), /*priority=*/128, now));
}

TEST(ReviverSubject, HigherPriorityWins) {
  ReviverSubject subject;
  const auto now = TimePoint{} + std::chrono::seconds(1);
  // Low-priority pinger arrives first and is active while alone.
  EXPECT_TRUE(subject.RecordPingAndIsActive(Addr(1), /*priority=*/100, now));
  // A higher-priority pinger takes the designation; the low one is no longer it.
  EXPECT_TRUE(subject.RecordPingAndIsActive(Addr(2), /*priority=*/200, now));
  EXPECT_FALSE(subject.RecordPingAndIsActive(Addr(1), /*priority=*/100, now));
}

TEST(ReviverSubject, EqualPriorityBreaksTowardLowestAddress) {
  ReviverSubject subject;
  const auto now = TimePoint{} + std::chrono::seconds(1);
  EXPECT_TRUE(subject.RecordPingAndIsActive(Addr(5), /*priority=*/128, now));
  // Same priority, lower address (Addr(3) < Addr(5)) deterministically wins so
  // every subject converges on the same monitor.
  EXPECT_TRUE(subject.RecordPingAndIsActive(Addr(3), /*priority=*/128, now));
  EXPECT_FALSE(subject.RecordPingAndIsActive(Addr(5), /*priority=*/128, now));
}

TEST(ReviverSubject, StalePingerAgesOutAndSurvivorTakesOver) {
  ReviverSubject subject;
  subject.SetTimeout(std::chrono::duration_cast<Duration>(std::chrono::seconds(1)));
  const auto t0 = TimePoint{} + std::chrono::seconds(10);
  // High-priority monitor is active; a low-priority standby stands by.
  EXPECT_TRUE(subject.RecordPingAndIsActive(Addr(1), /*priority=*/200, t0));
  EXPECT_FALSE(subject.RecordPingAndIsActive(Addr(2), /*priority=*/100, t0));
  EXPECT_EQ(subject.LivePingerCount(t0), 2u);

  // The high-priority monitor stops pinging; after the timeout it ages out and
  // the surviving standby becomes the active monitor.
  const auto t1 = t0 + std::chrono::seconds(2);
  EXPECT_TRUE(subject.RecordPingAndIsActive(Addr(2), /*priority=*/100, t1));
  EXPECT_EQ(subject.LivePingerCount(t1), 1u);
}

}  // namespace
}  // namespace atlas
