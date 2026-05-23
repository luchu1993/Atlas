// CellAppMgr message roundtrip tests.

#include <cmath>
#include <limits>
#include <optional>

#include <gtest/gtest.h>

#include "cellapp/cell_bounds.h"
#include "cellappmgr/cellappmgr_messages.h"
#include "network/message_ids.h"
#include "serialization/binary_stream.h"

using namespace atlas;
using namespace atlas::cellappmgr;

namespace {

template <typename Msg>
auto RoundTrip(const Msg& msg) -> std::optional<Msg> {
  BinaryWriter w;
  msg.Serialize(w);
  auto buf = w.Detach();
  BinaryReader r(buf);
  auto rt = Msg::Deserialize(r);
  if (!rt) return std::nullopt;
  return std::move(*rt);
}

}  // namespace

// ─── CellBounds (value type) ─────────────────────────────────────────────────

TEST(CellBounds, Defaults) {
  CellBounds b;
  EXPECT_TRUE(b.Contains(0.f, 0.f));
  EXPECT_TRUE(b.Contains(-1e9f, 1e9f));
  // Infinity edges imply everything is inside.
  EXPECT_TRUE(std::isinf(b.min_x));
  EXPECT_TRUE(std::isinf(b.max_x));
}

TEST(CellBounds, ContainsAndOverlaps) {
  CellBounds a{0.f, 0.f, 10.f, 10.f};
  CellBounds b{10.f, 0.f, 20.f, 10.f};  // shares max_x edge with a
  CellBounds c{5.f, 5.f, 15.f, 15.f};   // genuine overlap

  // Half-open on max: corner (10,5) belongs to b, not a.
  EXPECT_TRUE(b.Contains(10.f, 5.f));
  EXPECT_FALSE(a.Contains(10.f, 5.f));

  // Edge-sharing neighbours do NOT overlap.
  EXPECT_FALSE(a.Overlaps(b));
  EXPECT_TRUE(a.Overlaps(c));
}

TEST(CellBounds, RoundTripFiniteAndInfinite) {
  CellBounds b;
  b.min_x = -1000.f;
  b.min_z = -std::numeric_limits<float>::infinity();
  b.max_x = 1000.f;
  b.max_z = std::numeric_limits<float>::infinity();

  BinaryWriter w;
  b.Serialize(w);
  auto buf = w.Detach();
  BinaryReader r(buf);
  auto rt = CellBounds::Deserialize(r);
  ASSERT_TRUE(rt.HasValue());
  EXPECT_FLOAT_EQ(rt->min_x, -1000.f);
  EXPECT_TRUE(std::isinf(rt->min_z));
  EXPECT_TRUE(rt->min_z < 0);
  EXPECT_TRUE(std::isinf(rt->max_z));
  EXPECT_TRUE(rt->max_z > 0);
}

// ─── Messages ────────────────────────────────────────────────────────────────

TEST(CellAppMgrMessages, RegisterCellApp_RoundTrip) {
  RegisterCellApp msg;
  msg.internal_addr = Address(0x7F000001u, 30000);
  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->internal_addr.Ip(), 0x7F000001u);
  EXPECT_EQ(rt->internal_addr.Port(), 30000u);
}

TEST(CellAppMgrMessages, RegisterCellAppAck_RoundTrip) {
  RegisterCellAppAck msg;
  msg.success = true;
  msg.app_id = 5;
  msg.game_time = 0x1122334455667788ull;
  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_TRUE(rt->success);
  EXPECT_EQ(rt->app_id, 5u);
  EXPECT_EQ(rt->game_time, 0x1122334455667788ull);
}

TEST(CellAppMgrMessages, InformCellLoad_RoundTrip) {
  InformCellLoad msg;
  msg.app_id = 2;
  msg.load = 0.66f;
  msg.entity_count = 12345;
  InformCellLoad::CellReport a;
  a.cell_id = 3;
  a.entity_count = 100;
  a.median_x = 12.5f;
  a.median_z = -7.f;
  a.geometry_version = 9;
  a.tick_load = 0.25f;
  a.script_tick_us = 25000;
  a.witness_count = 7;
  a.aoi_peer_count = 70;
  a.aoi_reliable_bytes = 1024;
  a.aoi_unreliable_bytes = 2048;
  a.backup_bytes = 4096;
  a.x_buckets = {0, 3, 7, 11, 13, 17, 19, 23};
  a.z_buckets = {23, 19, 17, 13, 11, 7, 3, 0};
  msg.cells.push_back(a);
  InformCellLoad::CellReport b;
  b.cell_id = 4;
  b.entity_count = 50;
  b.median_x = -3.f;
  b.median_z = 9.5f;
  b.geometry_version = 10;
  b.tick_load = 0.125f;
  b.script_tick_us = 12500;
  b.witness_count = 3;
  b.aoi_peer_count = 30;
  b.aoi_reliable_bytes = 8192;
  b.aoi_unreliable_bytes = 16384;
  b.backup_bytes = 32768;
  msg.cells.push_back(b);
  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->app_id, 2u);
  EXPECT_NEAR(rt->load, 0.66f, 1e-5f);
  EXPECT_EQ(rt->entity_count, 12345u);
  ASSERT_EQ(rt->cells.size(), 2u);
  EXPECT_EQ(rt->cells[0].cell_id, 3u);
  EXPECT_EQ(rt->cells[0].entity_count, 100u);
  EXPECT_FLOAT_EQ(rt->cells[0].median_x, 12.5f);
  EXPECT_EQ(rt->cells[0].geometry_version, 9u);
  EXPECT_FLOAT_EQ(rt->cells[0].tick_load, 0.25f);
  EXPECT_EQ(rt->cells[0].script_tick_us, 25000u);
  EXPECT_EQ(rt->cells[0].witness_count, 7u);
  EXPECT_EQ(rt->cells[0].aoi_peer_count, 70u);
  EXPECT_EQ(rt->cells[0].aoi_reliable_bytes, 1024u);
  EXPECT_EQ(rt->cells[0].aoi_unreliable_bytes, 2048u);
  EXPECT_EQ(rt->cells[0].backup_bytes, 4096u);
  EXPECT_EQ(rt->cells[0].x_buckets[3], 11u);
  EXPECT_EQ(rt->cells[0].z_buckets[0], 23u);
  EXPECT_FLOAT_EQ(rt->cells[1].median_z, 9.5f);
  EXPECT_EQ(rt->cells[1].geometry_version, 10u);
  EXPECT_FLOAT_EQ(rt->cells[1].tick_load, 0.125f);
  EXPECT_EQ(rt->cells[1].script_tick_us, 12500u);
  EXPECT_EQ(rt->cells[1].witness_count, 3u);
  EXPECT_EQ(rt->cells[1].aoi_peer_count, 30u);
  EXPECT_EQ(rt->cells[1].aoi_reliable_bytes, 8192u);
  EXPECT_EQ(rt->cells[1].aoi_unreliable_bytes, 16384u);
  EXPECT_EQ(rt->cells[1].backup_bytes, 32768u);
}

TEST(CellAppMgrMessages, RequestCellAppState_RoundTrip) {
  RequestCellAppState msg;
  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
}

TEST(CellAppMgrMessages, CreateSpaceRequest_RoundTrip) {
  CreateSpaceRequest msg;
  msg.space_id = 999;
  msg.request_id = 42;
  msg.reply_addr = Address(0x0A000003u, 20003);
  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->space_id, 999u);
  EXPECT_EQ(rt->request_id, 42u);
  EXPECT_EQ(rt->reply_addr.Port(), 20003u);
}

TEST(CellAppMgrMessages, AddCellToSpace_RoundTrip) {
  AddCellToSpace msg;
  msg.space_id = 1;
  msg.cell_id = 7;
  msg.bounds = {-100.f, -100.f, 100.f, 100.f};
  msg.is_primary = true;
  msg.space_master_type = "SpaceMaster";
  msg.space_data_source_addr = Address(0x7F000001u, 30001);
  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->space_id, 1u);
  EXPECT_EQ(rt->cell_id, 7u);
  EXPECT_FLOAT_EQ(rt->bounds.min_x, -100.f);
  EXPECT_FLOAT_EQ(rt->bounds.max_z, 100.f);
  EXPECT_TRUE(rt->is_primary);
  EXPECT_EQ(rt->space_master_type, "SpaceMaster");
  EXPECT_EQ(rt->space_data_source_addr.Port(), 30001u);
}

TEST(CellAppMgrMessages, RemoveCellFromSpace_RoundTrip) {
  RemoveCellFromSpace msg;
  msg.space_id = 5;
  msg.cell_id = 9;
  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->space_id, 5u);
  EXPECT_EQ(rt->cell_id, 9u);
}

TEST(CellAppMgrMessages, UpdateGeometry_RoundTrip) {
  UpdateGeometry msg;
  msg.space_id = 77;
  msg.geometry_version = 1234;
  msg.bsp_blob = {std::byte{0xFE}, std::byte{0xED}, std::byte{0xFA}, std::byte{0xCE}};
  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->space_id, 77u);
  EXPECT_EQ(rt->geometry_version, 1234u);
  ASSERT_EQ(rt->bsp_blob.size(), 4u);
  EXPECT_EQ(rt->bsp_blob[0], std::byte{0xFE});
}

TEST(CellAppMgrMessages, UpdateGeometry_EmptyBlob) {
  UpdateGeometry msg;
  msg.space_id = 1;
  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_TRUE(rt->bsp_blob.empty());
}

TEST(CellAppMgrMessages, ShouldOffload_RoundTrip) {
  ShouldOffload msg;
  msg.space_id = 3;
  msg.cell_id = 4;
  msg.enable = false;
  msg.freeze_epoch = 99;
  auto rt = RoundTrip(msg);
  ASSERT_TRUE(rt.has_value());
  EXPECT_EQ(rt->space_id, 3u);
  EXPECT_EQ(rt->cell_id, 4u);
  EXPECT_FALSE(rt->enable);
  EXPECT_EQ(rt->freeze_epoch, 99u);
}

TEST(CellAppMgrMessages, MessageIdsAreStable) {
  EXPECT_EQ(msg_id::Id(msg_id::CellAppMgr::kRegisterCellApp), 7000u);
  EXPECT_EQ(msg_id::Id(msg_id::CellAppMgr::kRegisterCellAppAck), 7001u);
  EXPECT_EQ(msg_id::Id(msg_id::CellAppMgr::kInformCellLoad), 7002u);
  EXPECT_EQ(msg_id::Id(msg_id::CellAppMgr::kCreateSpaceRequest), 7003u);
  EXPECT_EQ(msg_id::Id(msg_id::CellAppMgr::kAddCellToSpace), 7004u);
  EXPECT_EQ(msg_id::Id(msg_id::CellAppMgr::kUpdateGeometry), 7005u);
  EXPECT_EQ(msg_id::Id(msg_id::CellAppMgr::kShouldOffload), 7006u);
  EXPECT_EQ(msg_id::Id(msg_id::CellAppMgr::kSpaceCreatedResult), 7007u);
  EXPECT_EQ(msg_id::Id(msg_id::CellAppMgr::kAddCellToSpaceAck), 7008u);
  EXPECT_EQ(msg_id::Id(msg_id::CellAppMgr::kRemoveCellFromSpace), 7009u);
  EXPECT_EQ(msg_id::Id(msg_id::CellAppMgr::kRequestCellAppState), 7010u);
}
