// BaseApp in-flight ClientCellRpc ordering across a cell-address change.
//
// During a teleport/offload the target entity briefly has no cell (CellAddr
// port 0). Client cell RPCs that arrive in that window must buffer and then
// drain to the new cell IN ORDER when OnCurrentCell installs the address.
// Teleport reuses this exact OnCurrentCell path, so this locks the guarantee.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "baseapp/baseapp.h"
#include "baseapp/baseapp_messages.h"
#include "cellapp/cellapp_messages.h"
#include "entitydef/entity_def_registry.h"
#include "entitydef/entity_type_descriptor.h"
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
  RecordingChannel(EventDispatcher& d, InterfaceTable& t, const Address& r) : Channel(d, t, r) {}
  [[nodiscard]] auto Fd() const -> FdHandle override { return kInvalidFd; }
  [[nodiscard]] auto DoSend(std::span<const std::byte> data) -> Result<size_t> override {
    sends_.emplace_back(data.begin(), data.end());
    return data.size();
  }
  [[nodiscard]] auto Sends() const -> const std::vector<std::vector<std::byte>>& { return sends_; }

 private:
  std::vector<std::vector<std::byte>> sends_;
};

// Cell-direction (bits 22-23 == 0b10), exposed to the owning client.
constexpr uint32_t kCellRpcId = 0x00800001u;

auto ForwardedRpcs(const RecordingChannel& ch) -> std::vector<cellapp::ClientCellRpcForward> {
  std::vector<cellapp::ClientCellRpcForward> out;
  for (const auto& frame : ch.Sends()) {
    BinaryReader reader(std::span<const std::byte>(frame.data(), frame.size()));
    const auto id = reader.ReadPackedInt();
    if (!id || *id != cellapp::ClientCellRpcForward::Descriptor().id) continue;
    const auto len = reader.ReadPackedInt();
    if (!len) continue;
    const auto payload = reader.ReadBytes(*len);
    if (!payload) continue;
    BinaryReader mr(*payload);
    auto msg = cellapp::ClientCellRpcForward::Deserialize(mr);
    if (msg.HasValue()) out.push_back(std::move(*msg));
  }
  return out;
}

}  // namespace

class BaseAppInflightRpcTest : public ::testing::Test {
 protected:
  BaseAppInflightRpcTest()
      : dispatcher_("baseapp_inflight"),
        internal_network_(dispatcher_),
        external_network_(dispatcher_),
        client_network_(dispatcher_),
        cell_addr_(0x7F000001u, 31000),
        app_(dispatcher_, internal_network_, external_network_),
        cell_ch_(make_intrusive<RecordingChannel>(dispatcher_, table_, cell_addr_)) {}

  void SetUp() override {
    dispatcher_.SetMaxPollWait(Milliseconds(1));
    EntityDefRegistry::Instance().clear();
    RegisterCellRpc();
    auto started = client_network_.StartRudpServer(Address("127.0.0.1", 0),
                                                   NetworkInterface::InternetRudpProfile());
    ASSERT_TRUE(started.HasValue()) << started.Error().Message();
    client_addr_ = client_network_.RudpAddress();
    client_ch_ = make_intrusive<RecordingChannel>(dispatcher_, table_, client_addr_);
  }

  void TearDown() override { EntityDefRegistry::Instance().clear(); }

  // Seed a client-bound entity that has NOT been placed on a cell yet, so
  // CellAddr().Port() == 0 and incoming cell RPCs buffer.
  auto SeedClientEntityWithoutCell() -> EntityID {
    app_.entity_mgr_.SetIdClient(&app_.id_client_);
    app_.id_client_.AddIds(1200, 1300);
    auto* entity = app_.entity_mgr_.Create(/*type_id=*/7, /*has_client=*/true);
    EXPECT_NE(entity, nullptr);
    app_.cellapp_peers_.InsertForTest(cell_addr_, cell_ch_.get());
    (void)external_network_.ConnectRudp(client_addr_);
    EXPECT_TRUE(app_.BindClient(entity->EntityId(), client_addr_));
    return entity->EntityId();
  }

  void SendCellRpc(EntityID target, std::byte tag) {
    baseapp::ClientCellRpc msg;
    msg.target_entity_id = target;
    msg.rpc_id = kCellRpcId;
    msg.payload = {tag};
    app_.OnClientCellRpc(*client_ch_, msg);
  }

  void DeliverCurrentCell(EntityID entity_id) {
    baseapp::CurrentCell cc;
    cc.entity_id = entity_id;
    cc.cell_addr = cell_addr_;
    cc.epoch = 1;
    app_.OnCurrentCell(*cell_ch_, cc);
  }

  EventDispatcher dispatcher_;
  NetworkInterface internal_network_;
  NetworkInterface external_network_;
  NetworkInterface client_network_;
  InterfaceTable table_;
  Address client_addr_{0x7F000001u, 0};
  Address cell_addr_;
  BaseApp app_;
  IntrusivePtr<RecordingChannel> cell_ch_;
  IntrusivePtr<RecordingChannel> client_ch_;

 private:
  static void RegisterCellRpc() {
    auto& reg = EntityDefRegistry::Instance();
    EntityTypeDescriptor type;
    RpcDescriptor rpc;
    rpc.rpc_id = kCellRpcId;
    rpc.exposed = ExposedScope::kOwnClient;
    type.rpcs.push_back(std::move(rpc));
    reg.types.push_back(std::move(type));
    reg.rpc_to_type[kCellRpcId] = reg.types.size() - 1;
  }
};

TEST_F(BaseAppInflightRpcTest, BuffersDuringNoCellThenDrainsInOrderOnCurrentCell) {
  const EntityID eid = SeedClientEntityWithoutCell();
  ASSERT_NE(eid, kInvalidEntityID);

  // No cell yet: these three RPCs must buffer, not forward.
  SendCellRpc(eid, std::byte{1});
  SendCellRpc(eid, std::byte{2});
  SendCellRpc(eid, std::byte{3});
  EXPECT_TRUE(ForwardedRpcs(*cell_ch_).empty()) << "RPCs forwarded before the cell was known";

  // CurrentCell installs the address and drains the buffer in arrival order.
  DeliverCurrentCell(eid);

  auto forwarded = ForwardedRpcs(*cell_ch_);
  ASSERT_EQ(forwarded.size(), 3u);
  EXPECT_EQ(forwarded[0].payload.at(0), std::byte{1});
  EXPECT_EQ(forwarded[1].payload.at(0), std::byte{2});
  EXPECT_EQ(forwarded[2].payload.at(0), std::byte{3});
  for (const auto& f : forwarded) {
    EXPECT_EQ(f.target_entity_id, eid);
    EXPECT_EQ(f.source_entity_id, eid);
    EXPECT_EQ(f.rpc_id, kCellRpcId);
  }
}

}  // namespace atlas
