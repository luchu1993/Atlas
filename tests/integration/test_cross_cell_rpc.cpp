// Cross-cell cell-method RPC: shooter cell holds a Ghost, target cell holds
// the Real. The Ghost->Real wire is cellapp::InternalCellRpc (already used
// by Base->Cell); CellAppNativeProvider::SendCellRpc looks up the Ghost,
// pulls real_channel_, and sends. Receiver's OnInternalCellRpc dispatches
// via dispatch_rpc_fn to C# DispatchCellRpc -> generated switch.
//
// Drives the end-to-end wire across two CellApps with a mock dispatch_rpc_fn
// on the receiver so the test does not need a live CLR script host.

#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "cell.h"
#include "cell_entity.h"
#include "cellapp.h"
#include "cellapp_messages.h"
#include "cellapp_native_provider.h"
#include "clrscript/native_api_provider.h"
#include "math/vector3.h"
#include "network/channel.h"
#include "network/event_dispatcher.h"
#include "network/interface_table.h"
#include "network/network_interface.h"
#include "network/reliable_udp.h"
#include "space.h"

namespace atlas {
namespace {

struct DispatchRecord {
  uint32_t entity_id;
  uint32_t rpc_id;
  int32_t payload_len;
  uint8_t first_byte;
};
std::vector<DispatchRecord>* g_dispatched = nullptr;

extern "C" void FakeDispatchRpc(uint32_t eid, uint32_t rid, intptr_t,
                                const uint8_t* payload, int32_t len, uint64_t) {
  if (g_dispatched) {
    g_dispatched->push_back({eid, rid, len, len > 0 ? payload[0] : uint8_t{0}});
  }
}

#pragma pack(push, 1)
struct CallbackTable {
  void* restore_entity;
  void* get_entity_data;
  void* entity_destroyed;
  void* dispatch_rpc;
  void* get_owner_snapshot;
  void* serialize_entity;
  void* proximity_event;
  void* coro_on_rpc_complete;
  void* entity_lifecycle_cancel;
  void* timer_event;
  void* entity_migrating_out;
  void* restore_ghost;
  void* destroy_ghost;
};
#pragma pack(pop)

struct Host {
  EventDispatcher dispatcher;
  NetworkInterface network;
  CellApp app;
  std::unique_ptr<INativeApiProvider> provider_holder;

  explicit Host(std::string name)
      : dispatcher(std::move(name)), network(dispatcher), app(dispatcher, network) {
    dispatcher.SetMaxPollWait(Milliseconds(1));
    auto& t = network.InterfaceTable();
    (void)t.RegisterTypedHandler<cellapp::InternalCellRpc>(
        [this](const Address& src, Channel* ch, const cellapp::InternalCellRpc& m) {
          app.OnInternalCellRpc(src, ch, m);
        });
  }

  auto StartServer() -> Address {
    auto r = network.StartRudpServer(Address("127.0.0.1", 0));
    EXPECT_TRUE(r.HasValue());
    return network.RudpAddress();
  }

  void WireProviderWithDispatch() {
    provider_holder = app.CreateNativeProviderForTest();
    CallbackTable table{};
    table.dispatch_rpc = reinterpret_cast<void*>(&FakeDispatchRpc);
    app.NativeProvider()->SetNativeCallbacks(&table, sizeof(table));
  }
};

auto PumpUntil(Host& a, Host& b, const std::function<bool()>& pred,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    a.dispatcher.ProcessOnce();
    b.dispatcher.ProcessOnce();
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

TEST(CrossCellRpc, GhostSendCellRpcReachesRealsDispatcher) {
  std::vector<DispatchRecord> dispatched;
  g_dispatched = &dispatched;

  Host shooter("shooter");
  Host target("target");
  auto shooter_addr = shooter.StartServer();
  auto target_addr = target.StartServer();

  shooter.WireProviderWithDispatch();
  target.WireProviderWithDispatch();

  cellapp::CreateCellEntity create_msg;
  create_msg.entity_id = 7000;
  create_msg.type_id = 1;
  create_msg.space_id = 1;
  create_msg.position = {5, 0, 5};
  create_msg.direction = {1, 0, 0};
  target.app.OnCreateCellEntity({}, nullptr, create_msg);
  ASSERT_NE(target.app.FindRealEntity(7000), nullptr);

  auto shooter_to_target = shooter.network.ConnectRudp(target_addr);
  ASSERT_TRUE(shooter_to_target.HasValue());
  shooter.app.PeerRegistryForTest().InsertForTest(target_addr, *shooter_to_target);

  cellapp::CreateSpace cs{1};
  shooter.app.OnCreateSpace({}, nullptr, cs);
  auto* shooter_space = shooter.app.FindSpace(1);
  ASSERT_NE(shooter_space, nullptr);

  auto ghost = std::make_unique<CellEntity>(
      CellEntity::GhostTag{}, /*id=*/7000, /*type_id=*/1, *shooter_space,
      math::Vector3{5, 0, 5}, math::Vector3{1, 0, 0},
      /*real_channel=*/static_cast<Channel*>(*shooter_to_target));
  auto* ghost_ptr = shooter_space->AddEntity(std::move(ghost));
  shooter.app.EntityPopulationForTest()[7000] = ghost_ptr;
  ASSERT_TRUE(ghost_ptr->IsGhost());

  // Payload that Npc.TakeDamage(int amount, uint attackerId) would deserialise
  // — Avatar_TakeDamage = 0x800204 in the codegen; this test uses a fixed rpc
  // id that the fake dispatcher captures verbatim (no real method dispatch).
  std::array<uint8_t, 8> payload{};
  const int32_t amount = 25;
  const uint32_t attacker = 42;
  std::memcpy(payload.data(), &amount, sizeof(amount));
  std::memcpy(payload.data() + 4, &attacker, sizeof(attacker));

  shooter.app.NativeProvider()->SendCellRpc(
      7000, 0x800401u, reinterpret_cast<const std::byte*>(payload.data()),
      static_cast<int32_t>(payload.size()), /*trace_id=*/0);

  const bool dispatched_ok =
      PumpUntil(shooter, target, [&]() { return !dispatched.empty(); });
  ASSERT_TRUE(dispatched_ok) << "InternalCellRpc did not reach target dispatcher";
  ASSERT_EQ(dispatched.size(), 1u);
  EXPECT_EQ(dispatched[0].entity_id, 7000u);
  EXPECT_EQ(dispatched[0].rpc_id, 0x800401u);
  EXPECT_EQ(dispatched[0].payload_len, 8);
  EXPECT_EQ(dispatched[0].first_byte, static_cast<uint8_t>(amount));

  g_dispatched = nullptr;
}

}  // namespace
}  // namespace atlas
