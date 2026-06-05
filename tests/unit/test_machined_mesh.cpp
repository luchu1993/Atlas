#include "server/machined_mesh.h"

#include <gtest/gtest.h>

#include "network/address.h"

namespace atlas {
namespace {

// Shared IP so the ring orders by port deterministically.
auto Addr(uint16_t port) -> Address { return Address(0x7F000001u, port); }

using Obs = MachinedMesh::Observation;

TEST(MachinedMesh, SoloMeshHasNoBuddyOrFailures) {
  MachinedMesh mesh(Addr(20));
  const auto now = TimePoint{} + std::chrono::seconds(1);
  EXPECT_FALSE(mesh.Buddy().has_value());
  EXPECT_TRUE(mesh.ScanFailures(now).owned.empty());
  EXPECT_EQ(mesh.KnownPeerCount(), 0u);
}

TEST(MachinedMesh, BuddyIsRingSuccessor) {
  MachinedMesh mesh(Addr(20));
  const auto now = TimePoint{} + std::chrono::seconds(1);
  mesh.RecordHeartbeat(Addr(10), /*incarnation=*/1, now);
  mesh.RecordHeartbeat(Addr(30), 1, now);
  mesh.RecordHeartbeat(Addr(40), 1, now);
  // Ring [10,20,30,40]; self=20 monitors its successor 30.
  ASSERT_TRUE(mesh.Buddy().has_value());
  EXPECT_EQ(*mesh.Buddy(), Addr(30));
}

TEST(MachinedMesh, BuddyWrapsAroundRing) {
  MachinedMesh mesh(Addr(40));
  const auto now = TimePoint{} + std::chrono::seconds(1);
  mesh.RecordHeartbeat(Addr(10), 1, now);
  mesh.RecordHeartbeat(Addr(20), 1, now);
  mesh.RecordHeartbeat(Addr(30), 1, now);
  // Ring [10,20,30,40]; the highest member wraps to monitor the lowest.
  ASSERT_TRUE(mesh.Buddy().has_value());
  EXPECT_EQ(*mesh.Buddy(), Addr(10));
}

TEST(MachinedMesh, RecordHeartbeatClassifiesNewKnownRestarted) {
  MachinedMesh mesh(Addr(20));
  const auto now = TimePoint{} + std::chrono::seconds(1);
  EXPECT_EQ(mesh.RecordHeartbeat(Addr(30), /*incarnation=*/7, now), Obs::kNew);
  EXPECT_EQ(mesh.RecordHeartbeat(Addr(30), 7, now), Obs::kKnown);
  EXPECT_EQ(mesh.RecordHeartbeat(Addr(30), 8, now), Obs::kRestarted);
}

TEST(MachinedMesh, RecordHeartbeatIgnoresSelf) {
  MachinedMesh mesh(Addr(20));
  const auto now = TimePoint{} + std::chrono::seconds(1);
  EXPECT_EQ(mesh.RecordHeartbeat(Addr(20), 1, now), Obs::kKnown);
  EXPECT_EQ(mesh.KnownPeerCount(), 0u);
}

TEST(MachinedMesh, ScanFailuresClaimsTimedOutBuddy) {
  MachinedMesh mesh(Addr(20));
  mesh.SetPeerTimeout(std::chrono::duration_cast<Duration>(std::chrono::seconds(1)));
  const auto t0 = TimePoint{} + std::chrono::seconds(10);
  mesh.RecordHeartbeat(Addr(30), 1, t0);  // becomes stale
  const auto t1 = t0 + std::chrono::seconds(2);
  mesh.RecordHeartbeat(Addr(10), 1, t1);
  mesh.RecordHeartbeat(Addr(40), 1, t1);

  const auto dead = mesh.ScanFailures(t1);
  ASSERT_EQ(dead.owned.size(), 1u);
  EXPECT_EQ(dead.owned[0], Addr(30));
  EXPECT_FALSE(mesh.Contains(Addr(30)));
  EXPECT_EQ(mesh.KnownPeerCount(), 2u);
  // The next live successor takes over as buddy.
  ASSERT_TRUE(mesh.Buddy().has_value());
  EXPECT_EQ(*mesh.Buddy(), Addr(40));
}

TEST(MachinedMesh, ScanFailuresWalksConsecutiveDeadRun) {
  MachinedMesh mesh(Addr(10));
  mesh.SetPeerTimeout(std::chrono::duration_cast<Duration>(std::chrono::seconds(1)));
  const auto t0 = TimePoint{} + std::chrono::seconds(10);
  mesh.RecordHeartbeat(Addr(20), 1, t0);  // stale
  mesh.RecordHeartbeat(Addr(30), 1, t0);  // stale
  const auto t1 = t0 + std::chrono::seconds(2);
  mesh.RecordHeartbeat(Addr(40), 1, t1);  // alive, stops the run

  const auto dead = mesh.ScanFailures(t1);
  ASSERT_EQ(dead.owned.size(), 2u);
  EXPECT_EQ(dead.owned[0], Addr(20));
  EXPECT_EQ(dead.owned[1], Addr(30));
  EXPECT_EQ(mesh.KnownPeerCount(), 1u);
  EXPECT_TRUE(mesh.Contains(Addr(40)));
}

TEST(MachinedMesh, ScanFailuresPrunesNonSuccessorDeathsWithoutClaimingThem) {
  MachinedMesh mesh(Addr(10));
  mesh.SetPeerTimeout(std::chrono::duration_cast<Duration>(std::chrono::seconds(1)));
  const auto t0 = TimePoint{} + std::chrono::seconds(10);
  mesh.RecordHeartbeat(Addr(30), 1, t0);  // stale, but owned by live peer 20
  const auto t1 = t0 + std::chrono::seconds(2);
  mesh.RecordHeartbeat(Addr(20), 1, t1);  // alive successor of self
  mesh.RecordHeartbeat(Addr(40), 1, t1);

  // self=10's successor 20 is alive, so self announces nothing, yet still drops
  // the stale 30 locally (its ring predecessor 20 owns announcing it).
  const auto dead = mesh.ScanFailures(t1);
  EXPECT_TRUE(dead.owned.empty());
  ASSERT_EQ(dead.pruned.size(), 1u);  // 30 is pruned locally though self doesn't own announcing it
  EXPECT_EQ(dead.pruned[0], Addr(30));
  EXPECT_FALSE(mesh.Contains(Addr(30)));
  EXPECT_EQ(mesh.KnownPeerCount(), 2u);
}

TEST(MachinedMesh, AliveBuddyYieldsNoFailures) {
  MachinedMesh mesh(Addr(10));
  mesh.SetPeerTimeout(std::chrono::duration_cast<Duration>(std::chrono::seconds(1)));
  const auto now = TimePoint{} + std::chrono::seconds(10);
  mesh.RecordHeartbeat(Addr(20), 1, now);
  EXPECT_TRUE(mesh.ScanFailures(now).owned.empty());
  EXPECT_EQ(mesh.KnownPeerCount(), 1u);
}

}  // namespace
}  // namespace atlas
