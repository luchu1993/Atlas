#include <cstdint>

#include <gtest/gtest.h>

#include "network/address.h"
#include "network/broadcast.h"

using namespace atlas;

static_assert(LimitedBroadcastAddress(20018).Ip() == kLimitedBroadcastIp);
static_assert(LimitedBroadcastAddress(20018).Port() == 20018);

TEST(Broadcast, LimitedBroadcastMatchesAllOnesAddress) {
  const Address expected("255.255.255.255", 20018);
  const Address actual = LimitedBroadcastAddress(20018);
  EXPECT_EQ(actual.Ip(), expected.Ip());
  EXPECT_EQ(actual.Port(), 20018);
}

TEST(Broadcast, DirectedBroadcastFillsHostBits) {
  const Address host("192.168.1.10", 0);
  const Address mask("255.255.255.0", 0);
  const Address expected("192.168.1.255", 20018);
  const Address actual = DirectedBroadcastAddress(host.Ip(), mask.Ip(), 20018);
  EXPECT_EQ(actual.Ip(), expected.Ip());
  EXPECT_EQ(actual.Port(), 20018);
}

TEST(Broadcast, DirectedBroadcastHonoursWiderPrefix) {
  const Address host("10.2.3.4", 0);
  const Address mask("255.255.0.0", 0);
  const Address expected("10.2.255.255", 0);
  EXPECT_EQ(DirectedBroadcastIp(host.Ip(), mask.Ip()), expected.Ip());
}
