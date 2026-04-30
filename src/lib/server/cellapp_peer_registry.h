#ifndef ATLAS_LIB_SERVER_CELLAPP_PEER_REGISTRY_H_
#define ATLAS_LIB_SERVER_CELLAPP_PEER_REGISTRY_H_

#include <cstddef>
#include <functional>
#include <unordered_map>

#include "network/address.h"

namespace atlas {

class Channel;
class MachinedClient;
class NetworkInterface;

// `self_addr` filters the registry owner out of its own peer list when
// machined re-broadcasts the owner's Birth. For callers that can never
// be their own peer (e.g. BaseApp), pass a default-constructed Address;
// the filter becomes a no-op.
// Thread safety: same as MachinedClient; must be used only from the
// EventDispatcher thread.

class CellAppPeerRegistry {
 public:
  explicit CellAppPeerRegistry(NetworkInterface& network);

  CellAppPeerRegistry(const CellAppPeerRegistry&) = delete;
  auto operator=(const CellAppPeerRegistry&) -> CellAppPeerRegistry& = delete;

  // Fired before the dying peer is erased from the registry.
  using PeerDeathHandler = std::function<void(const Address& addr, Channel* dying)>;

  // `on_death` runs before a recognized CellApp death is dropped.
  void Subscribe(MachinedClient& machined, Address self_addr, PeerDeathHandler on_death = {});

  [[nodiscard]] auto Find(const Address& addr) const -> Channel*;

  [[nodiscard]] auto Channels() const -> const std::unordered_map<Address, Channel*>& {
    return channels_;
  }

  [[nodiscard]] auto Size() const -> std::size_t { return channels_.size(); }

  // Test-only mutators; production writes come from Subscribe() callbacks.
  void InsertForTest(const Address& addr, Channel* ch);
  auto EraseForTest(const Address& addr) -> bool;

 private:
  NetworkInterface& network_;
  Address self_addr_;
  std::unordered_map<Address, Channel*> channels_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_SERVER_CELLAPP_PEER_REGISTRY_H_
