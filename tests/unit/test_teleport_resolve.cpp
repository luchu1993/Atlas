// CellAppMgr ResolveSpaceHostRequest -> ResolveSpaceHostReply tests.
// Verifies the mgr resolves (space, position) to the hosting cellapp leaf and
// reports found=false for an unhosted space.

#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "cellappmgr/cellappmgr.h"
#include "cellappmgr/cellappmgr_messages.h"
#include "network/address.h"
#include "network/channel.h"
#include "network/event_dispatcher.h"
#include "network/interface_table.h"
#include "network/network_interface.h"
#include "serialization/binary_stream.h"

namespace atlas {
namespace {

struct Harness {
  EventDispatcher dispatcher{"teleport-resolve-test"};
  NetworkInterface network{dispatcher};
  CellAppMgr mgr{dispatcher, network};
  Harness() { mgr.SetStartupQuiescenceWindowForTest(Duration::zero()); }
};

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

auto FirstReply(const RecordingChannel& ch) -> cellappmgr::ResolveSpaceHostReply {
  for (const auto& frame : ch.Sends()) {
    BinaryReader reader(std::span<const std::byte>(frame.data(), frame.size()));
    const auto id = reader.ReadPackedInt();
    if (!id || *id != cellappmgr::ResolveSpaceHostReply::Descriptor().id) continue;
    auto msg = cellappmgr::ResolveSpaceHostReply::Deserialize(reader);
    if (msg.HasValue()) return *msg;
  }
  ADD_FAILURE() << "No ResolveSpaceHostReply in sends";
  return {};
}

auto MakePeerAddr(uint16_t port) -> Address { return Address(0x7F000001u, port); }

void SeedHostedSpace(Harness& h, SpaceID space_id, const Address& peer) {
  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = peer;
  h.mgr.OnRegisterCellApp(peer, nullptr, reg);
  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = space_id;
  csr.request_id = 1;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);
}

TEST(TeleportResolve, HostedSpaceResolvesToLeaf) {
  Harness h;
  const Address peer = MakePeerAddr(30001);
  SeedHostedSpace(h, 42, peer);
  ASSERT_EQ(h.mgr.Spaces().count(42), 1u);

  EventDispatcher d{"rc"};
  InterfaceTable t;
  RecordingChannel ch(d, t, Address{});
  cellappmgr::ResolveSpaceHostRequest req;
  req.space_id = 42;
  req.position = {10.f, 0.f, 20.f};
  req.request_id = 7;
  req.entity_id = 1234;
  h.mgr.OnResolveSpaceHostRequest(peer, &ch, req);

  auto reply = FirstReply(ch);
  EXPECT_EQ(reply.request_id, 7u);
  EXPECT_EQ(reply.entity_id, 1234u);
  EXPECT_EQ(reply.space_id, 42u);
  EXPECT_TRUE(reply.found);
  EXPECT_EQ(reply.host_addr, peer);
  EXPECT_NE(reply.cell_id, 0u);
}

TEST(TeleportResolve, UnhostedSpaceReportsNotFound) {
  Harness h;
  EventDispatcher d{"rc"};
  InterfaceTable t;
  RecordingChannel ch(d, t, Address{});
  cellappmgr::ResolveSpaceHostRequest req;
  req.space_id = 999;
  req.position = {0.f, 0.f, 0.f};
  req.request_id = 8;
  req.entity_id = 5;
  h.mgr.OnResolveSpaceHostRequest(MakePeerAddr(30001), &ch, req);

  auto reply = FirstReply(ch);
  EXPECT_EQ(reply.request_id, 8u);
  EXPECT_FALSE(reply.found);
}

}  // namespace
}  // namespace atlas
