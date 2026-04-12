#include "network/address.hpp"
#include "server/ipv4_networks.hpp"

#include <gtest/gtest.h>

using namespace atlas;

TEST(IPv4NetworkSet, MatchesSingleHostAndCidrs)
{
    IPv4NetworkSet set;
    ASSERT_TRUE(set.add("127.0.0.1").has_value());
    ASSERT_TRUE(set.add("10.0.0.0/8").has_value());

    auto loopback = Address::resolve("127.0.0.1", 0);
    ASSERT_TRUE(loopback.has_value());
    EXPECT_TRUE(set.contains(loopback->ip()));

    auto private_ip = Address::resolve("10.42.7.9", 0);
    ASSERT_TRUE(private_ip.has_value());
    EXPECT_TRUE(set.contains(private_ip->ip()));

    auto public_ip = Address::resolve("8.8.8.8", 0);
    ASSERT_TRUE(public_ip.has_value());
    EXPECT_FALSE(set.contains(public_ip->ip()));
}

TEST(IPv4NetworkSet, RejectsInvalidSpecs)
{
    IPv4NetworkSet set;
    EXPECT_FALSE(set.add("").has_value());
    EXPECT_FALSE(set.add("127.0.0.1/99").has_value());
    EXPECT_FALSE(set.add("not-an-ip").has_value());
}
