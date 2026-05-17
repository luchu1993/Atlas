// Witness SpaceData envelope encoding tests.
//
// Wire format lock-in for kSpaceDataInit/Update/Delete — the C# SDK
// and the UE client both mirror these bytes, so any silent shift
// here desyncs every client.

#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "cell_aoi_envelope.h"
#include "cell_entity.h"
#include "math/vector3.h"
#include "space.h"
#include "space_data.h"
#include "witness.h"

namespace atlas {
namespace {

struct Captured {
  std::vector<std::byte> payload;
};

auto KindOf(const Captured& c) -> CellAoIEnvelopeKind {
  return static_cast<CellAoIEnvelopeKind>(c.payload.at(0));
}

auto ReadU16(const std::byte* p) -> uint16_t {
  uint16_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

auto ReadU32(const std::byte* p) -> uint32_t {
  uint32_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

class WitnessSpaceDataTest : public ::testing::Test {
 protected:
  std::vector<Captured> sent_;

  auto MakeReliable() {
    return [this](std::span<const std::byte> env) {
      sent_.push_back({std::vector<std::byte>(env.begin(), env.end())});
    };
  }

  auto MakeWitness(Space& space) -> Witness* {
    auto* e = space.AddEntity(std::make_unique<CellEntity>(
        1, /*type_id=*/uint16_t{1}, space, math::Vector3{0, 0, 0},
        math::Vector3{1, 0, 0}));
    e->SetBaseAddr(Address(0, 0));
    e->EnableWitness(50.f, MakeReliable(), Witness::SendFn{});
    return e->GetWitness();
  }
};

TEST_F(WitnessSpaceDataTest, SendUpdate_WireFormat) {
  Space space(/*id=*/7);
  auto* w = MakeWitness(space);
  sent_.clear();
  uint8_t value[] = {0xDE, 0xAD, 0xBE, 0xEF};
  w->SendSpaceDataUpdate(7, /*key_id=*/42, std::span<const uint8_t>(value, 4));

  ASSERT_EQ(sent_.size(), 1u);
  const auto& env = sent_[0].payload;
  EXPECT_EQ(KindOf(sent_[0]), CellAoIEnvelopeKind::kSpaceDataUpdate);
  EXPECT_EQ(ReadU32(&env[1]), 7u);     // space_id slot
  EXPECT_EQ(ReadU16(&env[5]), 42u);    // key_id
  EXPECT_EQ(ReadU32(&env[7]), 4u);     // vlen
  EXPECT_EQ(static_cast<uint8_t>(env[11]), 0xDE);
  EXPECT_EQ(static_cast<uint8_t>(env[14]), 0xEF);
}

TEST_F(WitnessSpaceDataTest, SendDelete_WireFormat) {
  Space space(/*id=*/9);
  auto* w = MakeWitness(space);
  sent_.clear();
  w->SendSpaceDataDelete(9, /*key_id=*/17);

  ASSERT_EQ(sent_.size(), 1u);
  const auto& env = sent_[0].payload;
  EXPECT_EQ(KindOf(sent_[0]), CellAoIEnvelopeKind::kSpaceDataDelete);
  EXPECT_EQ(ReadU32(&env[1]), 9u);
  EXPECT_EQ(ReadU16(&env[5]), 17u);
  EXPECT_EQ(env.size(), 1u + 4u + 2u);
}

TEST_F(WitnessSpaceDataTest, SendInit_OrderedByKeyId) {
  Space space(/*id=*/5);
  space.Data().Set(20, std::vector<uint8_t>{0xBB, 0xCC});
  space.Data().Set(10, std::vector<uint8_t>{0xAA});
  auto* w = MakeWitness(space);
  sent_.clear();
  w->SendSpaceDataInit(5, space.Data());

  ASSERT_EQ(sent_.size(), 1u);
  const auto& env = sent_[0].payload;
  EXPECT_EQ(KindOf(sent_[0]), CellAoIEnvelopeKind::kSpaceDataInit);
  EXPECT_EQ(ReadU32(&env[1]), 5u);
  EXPECT_EQ(ReadU32(&env[5]), 2u);            // count
  // First entry (sorted: id=10, vlen=1)
  EXPECT_EQ(ReadU16(&env[9]), 10u);
  EXPECT_EQ(ReadU32(&env[11]), 1u);
  EXPECT_EQ(static_cast<uint8_t>(env[15]), 0xAA);
  // Second entry (id=20, vlen=2)
  EXPECT_EQ(ReadU16(&env[16]), 20u);
  EXPECT_EQ(ReadU32(&env[18]), 2u);
  EXPECT_EQ(static_cast<uint8_t>(env[22]), 0xBB);
  EXPECT_EQ(static_cast<uint8_t>(env[23]), 0xCC);
}

TEST_F(WitnessSpaceDataTest, SendInit_EmptyDataEmitsZeroCount) {
  Space space(/*id=*/3);
  auto* w = MakeWitness(space);
  sent_.clear();
  w->SendSpaceDataInit(3, space.Data());

  ASSERT_EQ(sent_.size(), 1u);
  const auto& env = sent_[0].payload;
  EXPECT_EQ(KindOf(sent_[0]), CellAoIEnvelopeKind::kSpaceDataInit);
  EXPECT_EQ(ReadU32(&env[1]), 3u);
  EXPECT_EQ(ReadU32(&env[5]), 0u);
  EXPECT_EQ(env.size(), 1u + 4u + 4u);
}

}  // namespace
}  // namespace atlas
