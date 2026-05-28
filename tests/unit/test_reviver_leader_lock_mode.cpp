#include <gtest/gtest.h>

#include "reviver/reviver.h"

namespace atlas {
namespace {

TEST(ReviverLeaderLockMode, EmptyFallsBackToLocal) {
  EXPECT_EQ(NormalizeLeaderLockMode(""), "local");
}

TEST(ReviverLeaderLockMode, LocalIsCanonical) {
  EXPECT_EQ(NormalizeLeaderLockMode("local"), "local");
}

TEST(ReviverLeaderLockMode, MachinedIsCanonical) {
  EXPECT_EQ(NormalizeLeaderLockMode("machined"), "machined");
}

TEST(ReviverLeaderLockMode, TypoFallsBackToLocal) {
  // A common typo (`machind` for `machined`) must NOT silently enable the
  // machined backend — the InitTarget contract requires unknown values fall
  // back to local, with the caller logging the raw input so ops can spot
  // the misconfiguration.
  EXPECT_EQ(NormalizeLeaderLockMode("machind"), "local");
  EXPECT_EQ(NormalizeLeaderLockMode("MACHINED"), "local");
  EXPECT_EQ(NormalizeLeaderLockMode("file"), "local");
}

}  // namespace
}  // namespace atlas
