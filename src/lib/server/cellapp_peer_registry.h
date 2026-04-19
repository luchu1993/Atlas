#ifndef ATLAS_LIB_SERVER_CELLAPP_PEER_REGISTRY_H_
#define ATLAS_LIB_SERVER_CELLAPP_PEER_REGISTRY_H_

#include <cstddef>
#include <unordered_map>

#include "network/address.h"

namespace atlas {

class Channel;
class MachinedClient;
class NetworkInterface;

// ============================================================================
// CellAppPeerRegistry — shared map of {Address → Channel*} for peer
// CellApps, maintained by machined Birth/Death subscription.
//
// Phase 11 PR-6 review-fix C2. Previously BaseApp and CellApp each
// maintained a private `cellapp_channels_` / `peer_cellapp_channels_`
// map with duplicated Birth/Death logic. The registry consolidates both
// into one class so future policy changes (rate-limiting, signed
// envelope whitelists, etc.) land in a single place.
//
// `self_addr` filters the registry owner out of its own peer list when
// machined re-broadcasts the owner's Birth. For callers that can never
// be their own peer (e.g. BaseApp), pass a default-constructed Address;
// the filter becomes a no-op.
//
// Thread safety: same as MachinedClient — must be used only from the
// EventDispatcher thread.
// ============================================================================

class CellAppPeerRegistry {
 public:
  explicit CellAppPeerRegistry(NetworkInterface& network);

  CellAppPeerRegistry(const CellAppPeerRegistry&) = delete;
  auto operator=(const CellAppPeerRegistry&) -> CellAppPeerRegistry& = delete;

  // Installs Birth/Death handlers on machined for ProcessType::kCellApp.
  // Call once per process, typically from Init() after MachinedClient
  // has been connected AND NetworkInterface has bound its RUDP port
  // (so `self_addr` is known for the self-filter). Callers that can
  // never be their own peer — BaseApp, DBApp — pass a default Address.
  void Subscribe(MachinedClient& machined, Address self_addr);

  // Returns the peer's channel or nullptr if not currently registered.
  [[nodiscard]] auto Find(const Address& addr) const -> Channel*;

  // Direct map access — callers that need to iterate (e.g. broadcast
  // UpdateGeometry) or hand the map to a pure-function helper.
  [[nodiscard]] auto Channels() const -> const std::unordered_map<Address, Channel*>& {
    return channels_;
  }

  [[nodiscard]] auto Size() const -> std::size_t { return channels_.size(); }

  // Test-only mutators so unit tests can drive the routing helpers
  // without a real machined. Production writers are the subscription
  // callbacks installed by Subscribe(); no other path writes here.
  void InsertForTest(const Address& addr, Channel* ch);
  auto EraseForTest(const Address& addr) -> bool;

 private:
  NetworkInterface& network_;
  Address self_addr_;  // latched by Subscribe(); Address{} disables self-filter
  std::unordered_map<Address, Channel*> channels_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_SERVER_CELLAPP_PEER_REGISTRY_H_
