#include <unordered_map>

#include <gtest/gtest.h>

#include "network/address.h"

// Need sockaddr_in for the ToSockaddrAndBack test
#if defined(_WIN32)
#include <winsock2.h>
#else
#include <netinet/in.h>
#endif

using namespace atlas;

TEST(Address, DefaultIsNone) {
  Address addr;
  EXPECT_EQ(addr.Ip(), 0u);
  EXPECT_EQ(addr.Port(), 0u);
  EXPECT_EQ(addr, Address::kNone);
}

TEST(Address, ConstructFromStringAndPort) {
  Address addr("127.0.0.1", 8080);
  EXPECT_NE(addr.Ip(), 0u);
  EXPECT_EQ(addr.Port(), 8080);
}

TEST(Address, ToStringRoundTrip) {
  Address addr("192.168.1.100", 12345);
  auto str = addr.ToString();
  EXPECT_EQ(str, "192.168.1.100:12345");
}

TEST(Address, LoopbackAddress) {
  Address addr("127.0.0.1", 80);
  auto str = addr.ToString();
  EXPECT_EQ(str, "127.0.0.1:80");
}

TEST(Address, ComparisonOperators) {
  Address a("127.0.0.1", 80);
  Address b("127.0.0.1", 80);
  Address c("127.0.0.1", 81);
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
  // Ordering
  EXPECT_TRUE(a < c || a > c);  // different ports, one must be less
}

TEST(Address, HashInUnorderedMap) {
  std::unordered_map<Address, int> map;
  Address a("10.0.0.1", 100);
  Address b("10.0.0.2", 200);
  map[a] = 1;
  map[b] = 2;
  EXPECT_EQ(map[a], 1);
  EXPECT_EQ(map[b], 2);
  EXPECT_EQ(map.size(), 2u);
}

TEST(Address, ToSockaddrAndBack) {
  Address orig("192.168.0.1", 5000);
  auto sa = orig.ToSockaddr();
  Address reconstructed(sa);
  EXPECT_EQ(orig, reconstructed);
}

TEST(Address, ResolveLocalhost) {
  auto result = Address::Resolve("127.0.0.1", 8080);
  ASSERT_TRUE(result.HasValue());
  EXPECT_EQ(result->Port(), 8080);
  EXPECT_EQ(result->ToString(), "127.0.0.1:8080");
}
