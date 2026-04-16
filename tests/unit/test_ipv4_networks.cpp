#include <gtest/gtest.h>

#include "network/address.h"
#include "server/ipv4_networks.h"

using namespace atlas;

TEST(IPv4NetworkSet, MatchesSingleHostAndCidrs) {
  IPv4NetworkSet set;
  ASSERT_TRUE(set.Add("127.0.0.1").HasValue());
  ASSERT_TRUE(set.Add("10.0.0.0/8").HasValue());

  auto loopback = Address::Resolve("127.0.0.1", 0);
  ASSERT_TRUE(loopback.HasValue());
  EXPECT_TRUE(set.contains(loopback->Ip()));

  auto private_ip = Address::Resolve("10.42.7.9", 0);
  ASSERT_TRUE(private_ip.HasValue());
  EXPECT_TRUE(set.contains(private_ip->Ip()));

  auto public_ip = Address::Resolve("8.8.8.8", 0);
  ASSERT_TRUE(public_ip.HasValue());
  EXPECT_FALSE(set.contains(public_ip->Ip()));
}

TEST(IPv4NetworkSet, RejectsInvalidSpecs) {
  IPv4NetworkSet set;
  EXPECT_FALSE(set.Add("").HasValue());
  EXPECT_FALSE(set.Add("127.0.0.1/99").HasValue());
  EXPECT_FALSE(set.Add("not-an-ip").HasValue());
}
