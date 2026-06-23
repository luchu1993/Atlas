#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "baseapp/baseapp.h"
#include "baseapp/baseapp_messages.h"
#include "cellapp/cellapp_messages.h"
#include "entitydef/entity_def_registry.h"
#include "entitydef/entity_type_descriptor.h"
#include "foundation/clock.h"
#include "foundation/intrusive_ptr.h"
#include "network/channel.h"
#include "network/event_dispatcher.h"
#include "network/interface_table.h"
#include "network/network_interface.h"
#include "serialization/binary_stream.h"

namespace atlas {
namespace {

class RecordingChannel final : public Channel {
 public:
  RecordingChannel(EventDispatcher& dispatcher, InterfaceTable& table, const Address& remote)
      : Channel(dispatcher, table, remote) {}

  [[nodiscard]] auto Fd() const -> FdHandle override { return kInvalidFd; }

  [[nodiscard]] auto DoSend(std::span<const std::byte> data) -> Result<size_t> override {
    sends_.emplace_back(data.begin(), data.end());
    return data.size();
  }

  [[nodiscard]] auto Sends() const -> const std::vector<std::vector<std::byte>>& {
    return sends_;
  }

 private:
  std::vector<std::vector<std::byte>> sends_;
};

auto MovementForwards(const RecordingChannel& ch)
    -> std::vector<cellapp::ClientMovementInputForward> {
  std::vector<cellapp::ClientMovementInputForward> out;
  for (const auto& frame : ch.Sends()) {
    BinaryReader reader(std::span<const std::byte>(frame.data(), frame.size()));
    const auto id = reader.ReadPackedInt();
    if (!id || *id != cellapp::ClientMovementInputForward::Descriptor().id) continue;
    const auto len = reader.ReadPackedInt();
    if (!len) continue;
    const auto payload = reader.ReadBytes(*len);
    if (!payload) continue;
    BinaryReader msg_reader(*payload);
    auto msg = cellapp::ClientMovementInputForward::Deserialize(msg_reader);
    if (msg.HasValue()) out.push_back(std::move(*msg));
  }
  return out;
}

auto CreateCellEntities(const RecordingChannel& ch) -> std::vector<cellapp::CreateCellEntity> {
  std::vector<cellapp::CreateCellEntity> out;
  for (const auto& frame : ch.Sends()) {
    BinaryReader reader(std::span<const std::byte>(frame.data(), frame.size()));
    const auto id = reader.ReadPackedInt();
    if (!id || *id != cellapp::CreateCellEntity::Descriptor().id) continue;
    const auto len = reader.ReadPackedInt();
    if (!len) continue;
    const auto payload = reader.ReadBytes(*len);
    if (!payload) continue;
    BinaryReader msg_reader(*payload);
    auto msg = cellapp::CreateCellEntity::Deserialize(msg_reader);
    if (msg.HasValue()) out.push_back(std::move(*msg));
  }
  return out;
}

}  // namespace

class BaseAppMovementInputTest : public ::testing::Test {
 protected:
  BaseAppMovementInputTest()
      : dispatcher_("baseapp_movement"),
        internal_network_(dispatcher_),
        external_network_(dispatcher_),
        client_network_(dispatcher_),
        table_(),
        client_addr_(0x7F000001u, 0),
        cell_addr_(0x7F000001u, 31000),
        app_(dispatcher_, internal_network_, external_network_),
        client_ch_(nullptr),
        cell_ch_(make_intrusive<RecordingChannel>(dispatcher_, table_, cell_addr_)) {}

  void SetUp() override {
    EntityDefRegistry::Instance().clear();
    dispatcher_.SetMaxPollWait(Milliseconds(1));
    client_network_.InterfaceTable().RegisterTypedHandler<baseapp::MovementCommandStartToClient>(
        [this](const Address&, Channel*, const baseapp::MovementCommandStartToClient& msg) {
          movement_command_starts_.push_back(msg);
        });
    client_network_.InterfaceTable().RegisterTypedHandler<baseapp::MovementCommandEndToClient>(
        [this](const Address&, Channel*, const baseapp::MovementCommandEndToClient& msg) {
          movement_command_ends_.push_back(msg);
        });
    auto started = client_network_.StartRudpServer(Address("127.0.0.1", 0),
                                                  NetworkInterface::InternetRudpProfile());
    ASSERT_TRUE(started.HasValue()) << started.Error().Message();
    client_addr_ = client_network_.RudpAddress();
    client_ch_ = make_intrusive<RecordingChannel>(dispatcher_, table_, client_addr_);
  }

  void TearDown() override { EntityDefRegistry::Instance().clear(); }

  auto SeedClientEntity() -> EntityID {
    app_.entity_mgr_.SetIdClient(&app_.id_client_);
    app_.id_client_.AddIds(1200, 1300);
    auto* entity = app_.entity_mgr_.Create(/*type_id=*/7, /*has_client=*/true);
    if (entity == nullptr) return kInvalidEntityID;
    entity->SetCell(cell_addr_);
    app_.cellapp_peers_.InsertForTest(cell_addr_, cell_ch_.get());
    (void)external_network_.ConnectRudp(client_addr_);
    if (!app_.BindClient(entity->EntityId(), client_addr_)) return kInvalidEntityID;
    return entity->EntityId();
  }

  void RegisterCellBackedType(uint16_t type_id) {
    auto& reg = EntityDefRegistry::Instance();
    reg.clear();
    EntityTypeDescriptor type{};
    type.type_id = type_id;
    type.has_cell = true;
    type.has_client = false;
    reg.id_index[type_id] = reg.types.size();
    reg.types.push_back(std::move(type));
  }

  auto CreateScriptEntityAt(uint16_t type_id, SpaceID space_id, math::Vector3 position,
                            math::Vector3 direction, bool on_ground) -> EntityID {
    app_.entity_mgr_.SetIdClient(&app_.id_client_);
    app_.id_client_.AddIds(2000, 2100);
    app_.cellapp_peers_.InsertForTest(cell_addr_, cell_ch_.get());
    BaseAppNativeProvider provider(app_);
    return provider.CreateBaseEntityAt(type_id, space_id, position.x, position.y, position.z,
                                       direction.x, direction.y, direction.z, on_ground);
  }

  void OnClientMovementInput(const baseapp::ClientMovementInput& msg) {
    app_.OnClientMovementInput(*client_ch_, msg);
  }

  void OnMovementStateAckFromCell(const baseapp::MovementStateAckFromCell& msg) {
    app_.OnMovementStateAckFromCell(*cell_ch_, msg);
  }

  void OnMovementStateAckFromCell(Channel& ch, const baseapp::MovementStateAckFromCell& msg) {
    app_.OnMovementStateAckFromCell(ch, msg);
  }

  void OnMovementCommandStartFromCell(const baseapp::MovementCommandStartFromCell& msg) {
    app_.OnMovementCommandStartFromCell(*cell_ch_, msg);
  }

  void OnMovementCommandStartFromCell(Channel& ch,
                                      const baseapp::MovementCommandStartFromCell& msg) {
    app_.OnMovementCommandStartFromCell(ch, msg);
  }

  void OnMovementCommandEndFromCell(const baseapp::MovementCommandEndFromCell& msg) {
    app_.OnMovementCommandEndFromCell(*cell_ch_, msg);
  }

  void OnMovementCommandEndFromCell(Channel& ch,
                                    const baseapp::MovementCommandEndFromCell& msg) {
    app_.OnMovementCommandEndFromCell(ch, msg);
  }

  void OnMovementCorrectionReport(const baseapp::MovementCorrectionReport& msg) {
    app_.OnMovementCorrectionReport(*client_ch_, msg);
  }

  void SetCellEpoch(EntityID entity_id, uint32_t epoch) {
    auto* entity = app_.entity_mgr_.Find(entity_id);
    ASSERT_NE(entity, nullptr);
    entity->SetCell(cell_addr_, epoch);
  }

  void DestroyEntity(EntityID entity_id) {
    app_.entity_mgr_.Destroy(entity_id);
    app_.SweepMovementAckRelayState();
  }

  auto MovementAckRelayStateCount() const -> std::size_t {
    return app_.movement_ack_relay_state_.size();
  }

  auto PumpUntilMovementCommandStarts(std::size_t count) -> bool {
    for (int i = 0; i < 200; ++i) {
      dispatcher_.ProcessOnce();
      if (movement_command_starts_.size() >= count) return true;
    }
    return movement_command_starts_.size() >= count;
  }

  auto PumpUntilMovementCommandEnds(std::size_t count) -> bool {
    for (int i = 0; i < 200; ++i) {
      dispatcher_.ProcessOnce();
      if (movement_command_ends_.size() >= count) return true;
    }
    return movement_command_ends_.size() >= count;
  }

  void RegisterWatchers() { app_.RegisterWatchers(); }
  auto Watcher(std::string_view path) -> std::optional<std::string> {
    return app_.GetWatcherRegistry().Get(path);
  }

  EventDispatcher dispatcher_;
  NetworkInterface internal_network_;
  NetworkInterface external_network_;
  NetworkInterface client_network_;
  InterfaceTable table_;
  Address client_addr_;
  Address cell_addr_;
  BaseApp app_;
  IntrusivePtr<RecordingChannel> client_ch_;
  IntrusivePtr<RecordingChannel> cell_ch_;
  std::vector<baseapp::MovementCommandStartToClient> movement_command_starts_;
  std::vector<baseapp::MovementCommandEndToClient> movement_command_ends_;
};

TEST_F(BaseAppMovementInputTest, ForwardsStampedMovementInputToCurrentCell) {
  const EntityID entity_id = SeedClientEntity();
  ASSERT_NE(entity_id, kInvalidEntityID);

  baseapp::ClientMovementInput msg;
  msg.target_entity_id = entity_id;
  msg.frames.push_back({42, 100, 0, 127, 0, 0, 0, 16});

  OnClientMovementInput(msg);

  auto forwards = MovementForwards(*cell_ch_);
  ASSERT_EQ(forwards.size(), 1u);
  EXPECT_EQ(forwards[0].source_entity_id, entity_id);
  EXPECT_EQ(forwards[0].target_entity_id, entity_id);
  ASSERT_EQ(forwards[0].frames.size(), 1u);
  EXPECT_EQ(forwards[0].frames[0].seq, 42u);
}

TEST_F(BaseAppMovementInputTest, CreateBaseEntityAtSeedsInitialCellPose) {
  constexpr uint16_t kTypeId = 7;
  constexpr SpaceID kSpaceId = 5;
  const math::Vector3 position{300.f, 1.5f, -300.f};
  const math::Vector3 direction{0.f, 0.f, 1.f};
  RegisterCellBackedType(kTypeId);

  const EntityID entity_id =
      CreateScriptEntityAt(kTypeId, kSpaceId, position, direction, /*on_ground=*/true);

  ASSERT_NE(entity_id, kInvalidEntityID);
  auto creates = CreateCellEntities(*cell_ch_);
  ASSERT_EQ(creates.size(), 1u);
  EXPECT_EQ(creates[0].entity_id, entity_id);
  EXPECT_EQ(creates[0].type_id, kTypeId);
  EXPECT_EQ(creates[0].space_id, kSpaceId);
  EXPECT_FLOAT_EQ(creates[0].position.x, position.x);
  EXPECT_FLOAT_EQ(creates[0].position.y, position.y);
  EXPECT_FLOAT_EQ(creates[0].position.z, position.z);
  EXPECT_FLOAT_EQ(creates[0].direction.x, direction.x);
  EXPECT_FLOAT_EQ(creates[0].direction.y, direction.y);
  EXPECT_FLOAT_EQ(creates[0].direction.z, direction.z);
  EXPECT_TRUE(creates[0].on_ground);
}

TEST_F(BaseAppMovementInputTest, RejectsCrossEntityMovementInput) {
  const EntityID entity_id = SeedClientEntity();
  ASSERT_NE(entity_id, kInvalidEntityID);

  baseapp::ClientMovementInput msg;
  msg.target_entity_id = entity_id + 1;
  msg.frames.push_back({42, 100, 0, 127, 0, 0, 0, 16});

  OnClientMovementInput(msg);

  EXPECT_TRUE(MovementForwards(*cell_ch_).empty());
}

TEST_F(BaseAppMovementInputTest, RejectsInvalidClientDt) {
  const EntityID entity_id = SeedClientEntity();
  ASSERT_NE(entity_id, kInvalidEntityID);
  RegisterWatchers();

  baseapp::ClientMovementInput msg;
  msg.target_entity_id = entity_id;
  msg.frames.push_back({42, 100, 0, 127, 0, 0, 0, 0});
  OnClientMovementInput(msg);

  msg.frames.clear();
  msg.frames.push_back({43, 101, 0, 127, 0, 0, 0, 1000});
  OnClientMovementInput(msg);

  EXPECT_TRUE(MovementForwards(*cell_ch_).empty());
  EXPECT_EQ(Watcher("movement/input_invalid_dropped_total").value_or(""), "2");
  EXPECT_EQ(Watcher("movement/input_dropped_total").value_or(""), "2");
}

TEST_F(BaseAppMovementInputTest, MovementInputRateLimitHasDedicatedWatcher) {
  const EntityID entity_id = SeedClientEntity();
  ASSERT_NE(entity_id, kInvalidEntityID);
  RegisterWatchers();

  baseapp::ClientMovementInput msg;
  msg.target_entity_id = entity_id;
  for (uint32_t seq = 1; seq <= 400; ++seq) {
    msg.frames.clear();
    msg.frames.push_back({seq, seq, 0, 127, 0, 0, 0, 16});
    OnClientMovementInput(msg);
  }

  EXPECT_NE(Watcher("movement/input_rate_limited_total").value_or("0"), "0");
}

TEST_F(BaseAppMovementInputTest, DropsStaleAndLargeGapMovementSeq) {
  const EntityID entity_id = SeedClientEntity();
  ASSERT_NE(entity_id, kInvalidEntityID);
  RegisterWatchers();

  baseapp::ClientMovementInput msg;
  msg.target_entity_id = entity_id;
  msg.frames.push_back({10, 100, 0, 127, 0, 0, 0, 16});
  OnClientMovementInput(msg);
  EXPECT_EQ(MovementForwards(*cell_ch_).size(), 1u);
  EXPECT_EQ(Watcher("movement/input_stale_dropped_total").value_or("0"), "0");

  OnClientMovementInput(msg);
  EXPECT_EQ(MovementForwards(*cell_ch_).size(), 1u);
  EXPECT_EQ(Watcher("movement/input_stale_dropped_total").value_or("0"), "1");

  msg.frames.clear();
  msg.frames.push_back({9, 101, 0, 127, 0, 0, 0, 16});
  msg.frames.push_back({10, 102, 0, 127, 0, 0, 0, 16});
  msg.frames.push_back({11, 103, 0, 127, 0, 0, 0, 16});
  OnClientMovementInput(msg);
  auto forwards = MovementForwards(*cell_ch_);
  ASSERT_EQ(forwards.size(), 2u);
  EXPECT_EQ(forwards[1].frames.back().seq, 11u);

  msg.frames.clear();
  msg.frames.push_back({400, 104, 0, 127, 0, 0, 0, 16});
  OnClientMovementInput(msg);
  EXPECT_EQ(MovementForwards(*cell_ch_).size(), 2u);
  EXPECT_EQ(Watcher("movement/input_seq_gap_dropped_total").value_or("0"), "1");

  msg.frames.clear();
  msg.frames.push_back({12, 105, 0, 127, 0, 0, 0, 16});
  OnClientMovementInput(msg);
  forwards = MovementForwards(*cell_ch_);
  ASSERT_EQ(forwards.size(), 3u);
  EXPECT_EQ(forwards[2].frames.back().seq, 12u);
}

TEST_F(BaseAppMovementInputTest, MovementAckSeedsInputSequenceAfterReconnect) {
  const EntityID entity_id = SeedClientEntity();
  ASSERT_NE(entity_id, kInvalidEntityID);
  RegisterWatchers();

  baseapp::ClientMovementInput input;
  input.target_entity_id = entity_id;
  input.frames.push_back({1, 100, 0, 127, 0, 0, 0, 16});
  OnClientMovementInput(input);
  EXPECT_EQ(MovementForwards(*cell_ch_).size(), 1u);

  baseapp::MovementStateAckFromCell ack;
  ack.entity_id = entity_id;
  ack.acked_input_seq = 1000;
  ack.server_tick = 9000;
  ack.state.last_processed_input_seq = 1000;
  OnMovementStateAckFromCell(ack);

  input.frames.clear();
  input.frames.push_back({1001, 101, 0, 127, 0, 0, 0, 16});
  OnClientMovementInput(input);

  auto forwards = MovementForwards(*cell_ch_);
  ASSERT_EQ(forwards.size(), 2u);
  EXPECT_EQ(forwards[1].frames.back().seq, 1001u);
  EXPECT_EQ(Watcher("movement/input_seq_gap_dropped_total").value_or("0"), "0");
}

TEST_F(BaseAppMovementInputTest, RelaysMovementAckToOwnerClient) {
  const EntityID entity_id = SeedClientEntity();
  ASSERT_NE(entity_id, kInvalidEntityID);
  RegisterWatchers();

  baseapp::MovementStateAckFromCell msg;
  msg.entity_id = entity_id;
  msg.acked_input_seq = 42;
  msg.server_tick = 9001;
  msg.state.position = {1.0f, 2.0f, 3.0f};
  msg.state.velocity = {4.0f, 5.0f, 6.0f};
  msg.state.direction = {0.0f, 0.0f, 1.0f};
  msg.state.flags = movement::kMovementFlagGrounded;
  msg.state.last_processed_input_seq = 42;
  msg.correction_flags = movement::kCorrectionFlagTier1;

  OnMovementStateAckFromCell(msg);

  EXPECT_EQ(Watcher("movement/ack_sent_total").value_or("0"), "1");
  EXPECT_EQ(Watcher("movement/correction_tier1_total").value_or("0"), "1");

  msg.server_tick = 9002;
  msg.correction_flags = movement::kCorrectionFlagTier2;
  OnMovementStateAckFromCell(msg);
  EXPECT_EQ(Watcher("movement/ack_sent_total").value_or("0"), "2");
  EXPECT_EQ(Watcher("movement/correction_tier2_total").value_or("0"), "1");

  msg.server_tick = 9003;
  msg.correction_flags = movement::kCorrectionFlagSnap;
  OnMovementStateAckFromCell(msg);
  EXPECT_EQ(Watcher("movement/ack_sent_total").value_or("0"), "3");
  EXPECT_EQ(Watcher("movement/correction_snap_total").value_or("0"), "1");

  OnMovementStateAckFromCell(msg);
  EXPECT_EQ(Watcher("movement/ack_sent_total").value_or("0"), "3");
  EXPECT_EQ(Watcher("movement/ack_stale_dropped_total").value_or("0"), "1");

  msg.acked_input_seq = 41;
  msg.server_tick = 9004;
  OnMovementStateAckFromCell(msg);
  EXPECT_EQ(Watcher("movement/ack_sent_total").value_or("0"), "3");
  EXPECT_EQ(Watcher("movement/ack_stale_dropped_total").value_or("0"), "2");
}

TEST_F(BaseAppMovementInputTest, RelaysMovementCommandStartToDestClient) {
  const EntityID entity_id = SeedClientEntity();
  ASSERT_NE(entity_id, kInvalidEntityID);

  baseapp::MovementCommandStartFromCell msg;
  msg.source_entity_id = entity_id;
  msg.dest_entity_ids.push_back(entity_id);
  msg.command.command_id = 900;
  msg.command.skill_id = 12;
  msg.command.type = movement::MovementCommandType::kDash;
  msg.command.start_position = {1.f, 2.f, 3.f};
  msg.command.target_position = {4.f, 5.f, 6.f};
  msg.command.duration_ms = 700;
  msg.command.input_policy = movement::MovementCommandInputPolicy::kAllowTurn;
  msg.command.collision_policy = movement::MovementCommandCollisionPolicy::kStop;
  msg.command.priority = 9;
  msg.command.server_tick = 301;

  OnMovementCommandStartFromCell(msg);

  ASSERT_TRUE(PumpUntilMovementCommandStarts(1));
  EXPECT_EQ(movement_command_starts_[0].entity_id, entity_id);
  EXPECT_EQ(movement_command_starts_[0].command.command_id, 900u);
  EXPECT_EQ(movement_command_starts_[0].command.type, movement::MovementCommandType::kDash);
  EXPECT_FLOAT_EQ(movement_command_starts_[0].command.target_position.z, 6.f);
}

TEST_F(BaseAppMovementInputTest, DropsMovementCommandStartFromStaleCell) {
  const EntityID entity_id = SeedClientEntity();
  ASSERT_NE(entity_id, kInvalidEntityID);
  SetCellEpoch(entity_id, 2);

  baseapp::MovementCommandStartFromCell msg;
  msg.source_entity_id = entity_id;
  msg.cell_epoch = 1;
  msg.dest_entity_ids.push_back(entity_id);
  msg.command.command_id = 901;
  msg.command.type = movement::MovementCommandType::kDash;
  msg.command.duration_ms = 700;

  OnMovementCommandStartFromCell(msg);
  dispatcher_.ProcessOnce();
  EXPECT_TRUE(movement_command_starts_.empty());

  msg.cell_epoch = 2;
  OnMovementCommandStartFromCell(msg);
  EXPECT_TRUE(PumpUntilMovementCommandStarts(1));
}

TEST_F(BaseAppMovementInputTest, RelaysMovementCommandEndToDestClient) {
  const EntityID entity_id = SeedClientEntity();
  ASSERT_NE(entity_id, kInvalidEntityID);

  baseapp::MovementCommandEndFromCell msg;
  msg.source_entity_id = entity_id;
  msg.dest_entity_ids.push_back(entity_id);
  msg.command_id = 900;
  msg.server_tick = 302;
  msg.reason = movement::MovementCommandEndReason::kCollision;
  msg.state.position = {4.f, 5.f, 6.f};
  msg.state.direction = {0.f, 0.f, 1.f};
  msg.state.flags = movement::kMovementFlagGrounded;
  msg.state.last_processed_input_seq = 77;

  OnMovementCommandEndFromCell(msg);

  ASSERT_TRUE(PumpUntilMovementCommandEnds(1));
  EXPECT_EQ(movement_command_ends_[0].entity_id, entity_id);
  EXPECT_EQ(movement_command_ends_[0].command_id, 900u);
  EXPECT_EQ(movement_command_ends_[0].server_tick, 302u);
  EXPECT_EQ(movement_command_ends_[0].reason,
            movement::MovementCommandEndReason::kCollision);
  EXPECT_FLOAT_EQ(movement_command_ends_[0].state.position.z, 6.f);
}

TEST_F(BaseAppMovementInputTest, DropsMovementCommandEndFromStaleCell) {
  const EntityID entity_id = SeedClientEntity();
  ASSERT_NE(entity_id, kInvalidEntityID);
  SetCellEpoch(entity_id, 2);

  baseapp::MovementCommandEndFromCell msg;
  msg.source_entity_id = entity_id;
  msg.cell_epoch = 1;
  msg.dest_entity_ids.push_back(entity_id);
  msg.command_id = 901;
  msg.server_tick = 302;

  OnMovementCommandEndFromCell(msg);
  dispatcher_.ProcessOnce();
  EXPECT_TRUE(movement_command_ends_.empty());

  msg.cell_epoch = 2;
  OnMovementCommandEndFromCell(msg);
  EXPECT_TRUE(PumpUntilMovementCommandEnds(1));
}

TEST_F(BaseAppMovementInputTest, TracksConsecutiveLargeMovementCorrections) {
  const EntityID entity_id = SeedClientEntity();
  ASSERT_NE(entity_id, kInvalidEntityID);
  RegisterWatchers();

  baseapp::MovementStateAckFromCell msg;
  msg.entity_id = entity_id;
  msg.acked_input_seq = 80;
  msg.server_tick = 9400;
  msg.state.last_processed_input_seq = 80;
  msg.correction_flags = movement::kCorrectionFlagTier2;

  OnMovementStateAckFromCell(msg);
  EXPECT_EQ(Watcher("movement/correction_suspicious_total").value_or("0"), "0");

  msg.server_tick = 9401;
  OnMovementStateAckFromCell(msg);
  EXPECT_EQ(Watcher("movement/correction_suspicious_total").value_or("0"), "0");

  msg.server_tick = 9402;
  OnMovementStateAckFromCell(msg);
  EXPECT_EQ(Watcher("movement/correction_suspicious_total").value_or("0"), "1");

  msg.server_tick = 9403;
  msg.correction_flags = movement::kCorrectionFlagTier1;
  OnMovementStateAckFromCell(msg);

  msg.server_tick = 9404;
  msg.correction_flags = movement::kCorrectionFlagSnap;
  OnMovementStateAckFromCell(msg);
  EXPECT_EQ(Watcher("movement/correction_suspicious_total").value_or("0"), "1");
}

TEST_F(BaseAppMovementInputTest, AcceptsOwnerMovementCorrectionReportForRelayedAck) {
  const EntityID entity_id = SeedClientEntity();
  ASSERT_NE(entity_id, kInvalidEntityID);
  RegisterWatchers();

  baseapp::MovementStateAckFromCell ack;
  ack.entity_id = entity_id;
  ack.acked_input_seq = 90;
  ack.server_tick = 9500;
  ack.state.last_processed_input_seq = 90;
  OnMovementStateAckFromCell(ack);

  baseapp::MovementCorrectionReport report;
  report.target_entity_id = entity_id;
  report.acked_input_seq = 90;
  report.server_tick = 9500;
  report.distance_m = movement::kCorrectionTier2DistanceM;
  report.correction_flags = movement::kCorrectionFlagTier2;
  OnMovementCorrectionReport(report);

  EXPECT_EQ(Watcher("movement/correction_report_total").value_or("0"), "1");
  EXPECT_EQ(Watcher("movement/correction_report_dropped_total").value_or("0"), "0");
  EXPECT_EQ(Watcher("movement/correction_tier2_total").value_or("0"), "1");

  OnMovementCorrectionReport(report);
  EXPECT_EQ(Watcher("movement/correction_report_total").value_or("0"), "1");
  EXPECT_EQ(Watcher("movement/correction_report_dropped_total").value_or("0"), "1");
  EXPECT_EQ(Watcher("movement/correction_tier2_total").value_or("0"), "1");
}

TEST_F(BaseAppMovementInputTest, DropsInvalidMovementCorrectionReport) {
  const EntityID entity_id = SeedClientEntity();
  ASSERT_NE(entity_id, kInvalidEntityID);
  RegisterWatchers();

  baseapp::MovementStateAckFromCell ack;
  ack.entity_id = entity_id;
  ack.acked_input_seq = 91;
  ack.server_tick = 9600;
  ack.state.last_processed_input_seq = 91;
  OnMovementStateAckFromCell(ack);

  baseapp::MovementCorrectionReport report;
  report.target_entity_id = entity_id;
  report.acked_input_seq = 92;
  report.server_tick = 9601;
  report.distance_m = movement::kCorrectionTier2DistanceM;
  report.correction_flags = movement::kCorrectionFlagTier2;
  OnMovementCorrectionReport(report);

  report.acked_input_seq = 91;
  report.server_tick = 9600;
  report.correction_flags = movement::kCorrectionFlagTier1;
  OnMovementCorrectionReport(report);

  EXPECT_EQ(Watcher("movement/correction_report_total").value_or("0"), "0");
  EXPECT_EQ(Watcher("movement/correction_report_dropped_total").value_or("0"), "2");
}

TEST_F(BaseAppMovementInputTest, DropsMovementAckFromOlderCellEpoch) {
  const EntityID entity_id = SeedClientEntity();
  ASSERT_NE(entity_id, kInvalidEntityID);
  SetCellEpoch(entity_id, 2);
  RegisterWatchers();

  baseapp::MovementStateAckFromCell msg;
  msg.entity_id = entity_id;
  msg.acked_input_seq = 50;
  msg.server_tick = 9100;
  msg.cell_epoch = 1;
  msg.state.last_processed_input_seq = 50;

  OnMovementStateAckFromCell(msg);
  EXPECT_EQ(Watcher("movement/ack_sent_total").value_or("0"), "0");
  EXPECT_EQ(Watcher("movement/ack_stale_dropped_total").value_or("0"), "1");

  msg.cell_epoch = 2;
  OnMovementStateAckFromCell(msg);
  EXPECT_EQ(Watcher("movement/ack_sent_total").value_or("0"), "1");
  EXPECT_EQ(Watcher("movement/ack_stale_dropped_total").value_or("0"), "1");
}

TEST_F(BaseAppMovementInputTest, AcceptsMovementAckFromNewerCellEpoch) {
  const EntityID entity_id = SeedClientEntity();
  ASSERT_NE(entity_id, kInvalidEntityID);
  SetCellEpoch(entity_id, 1);
  RegisterWatchers();

  baseapp::MovementStateAckFromCell msg;
  msg.entity_id = entity_id;
  msg.acked_input_seq = 50;
  msg.server_tick = 9000;
  msg.cell_epoch = 1;
  msg.state.last_processed_input_seq = 50;

  OnMovementStateAckFromCell(msg);
  EXPECT_EQ(Watcher("movement/ack_sent_total").value_or("0"), "1");

  SetCellEpoch(entity_id, 2);
  msg.server_tick = 1;
  msg.cell_epoch = 2;
  OnMovementStateAckFromCell(msg);
  EXPECT_EQ(Watcher("movement/ack_sent_total").value_or("0"), "2");
  EXPECT_EQ(Watcher("movement/ack_stale_dropped_total").value_or("0"), "0");
}

TEST_F(BaseAppMovementInputTest, DropsMovementAckFromNonCurrentCell) {
  const EntityID entity_id = SeedClientEntity();
  ASSERT_NE(entity_id, kInvalidEntityID);
  RegisterWatchers();

  const Address stale_addr(0x7F000001u, 31001);
  auto stale_ch = make_intrusive<RecordingChannel>(dispatcher_, table_, stale_addr);

  baseapp::MovementStateAckFromCell msg;
  msg.entity_id = entity_id;
  msg.acked_input_seq = 60;
  msg.server_tick = 9200;
  msg.state.last_processed_input_seq = 60;

  OnMovementStateAckFromCell(*stale_ch, msg);
  EXPECT_EQ(Watcher("movement/ack_sent_total").value_or("0"), "0");
  EXPECT_EQ(Watcher("movement/ack_stale_dropped_total").value_or("0"), "1");

  OnMovementStateAckFromCell(msg);
  EXPECT_EQ(Watcher("movement/ack_sent_total").value_or("0"), "1");
}

TEST_F(BaseAppMovementInputTest, ClearsMovementAckRelayStateAfterEntityDestroy) {
  const EntityID entity_id = SeedClientEntity();
  ASSERT_NE(entity_id, kInvalidEntityID);

  baseapp::MovementStateAckFromCell msg;
  msg.entity_id = entity_id;
  msg.acked_input_seq = 70;
  msg.server_tick = 9300;
  msg.state.last_processed_input_seq = 70;

  OnMovementStateAckFromCell(msg);
  EXPECT_EQ(MovementAckRelayStateCount(), 1u);

  DestroyEntity(entity_id);
  EXPECT_EQ(MovementAckRelayStateCount(), 0u);
}

}  // namespace atlas
