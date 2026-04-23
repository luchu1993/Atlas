#include "cellapp_peer_registry.h"

#include "foundation/log.h"
#include "network/channel.h"
#include "network/machined_types.h"
#include "network/network_interface.h"
#include "network/reliable_udp.h"
#include "server/machined_client.h"

namespace atlas {

CellAppPeerRegistry::CellAppPeerRegistry(NetworkInterface& network) : network_(network) {}

void CellAppPeerRegistry::Subscribe(MachinedClient& machined, Address self_addr,
                                    PeerDeathHandler on_death) {
  self_addr_ = self_addr;
  machined.Subscribe(
      machined::ListenerType::kBoth, ProcessType::kCellApp,
      [this](const machined::BirthNotification& n) {
        // Filter self (CellApp receives its own birth notification).
        // BaseApp passes Address{} so this condition is never true for it.
        if (self_addr_.Ip() != 0 && n.internal_addr == self_addr_) return;
        ATLAS_LOG_INFO("CellAppPeerRegistry: CellApp born at {}:{}", n.internal_addr.Ip(),
                       n.internal_addr.Port());
        auto ch = network_.ConnectRudpNocwnd(n.internal_addr);
        if (ch) channels_.insert_or_assign(n.internal_addr, static_cast<Channel*>(*ch));
      },
      [this, on_death = std::move(on_death)](const machined::DeathNotification& n) {
        auto it = channels_.find(n.internal_addr);
        if (it == channels_.end()) return;
        Channel* dying = it->second;
        if (on_death) on_death(n.internal_addr, dying);
        channels_.erase(it);
        ATLAS_LOG_WARNING("CellAppPeerRegistry: CellApp died at {}:{}", n.internal_addr.Ip(),
                          n.internal_addr.Port());
      });
}

auto CellAppPeerRegistry::Find(const Address& addr) const -> Channel* {
  auto it = channels_.find(addr);
  return it == channels_.end() ? nullptr : it->second;
}

void CellAppPeerRegistry::InsertForTest(const Address& addr, Channel* ch) {
  if (ch == nullptr) {
    channels_.erase(addr);
  } else {
    channels_[addr] = ch;
  }
}

auto CellAppPeerRegistry::EraseForTest(const Address& addr) -> bool {
  return channels_.erase(addr) > 0;
}

}  // namespace atlas
