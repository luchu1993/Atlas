#ifndef ATLAS_SERVER_CELLAPP_CELLAPP_NATIVE_PROVIDER_H_
#define ATLAS_SERVER_CELLAPP_CELLAPP_NATIVE_PROVIDER_H_

#include <cstddef>
#include <cstdint>
#include <functional>

#include "baseapp/baseapp_native_provider.h"  // RestoreEntityFn, DispatchRpcFn, etc.
#include "clrscript/base_native_provider.h"

namespace atlas {

class CellEntity;
class NetworkInterface;

// ============================================================================
// CellAppNativeProvider — INativeApiProvider wired to a CellApp process
//
// The provider is a thin façade: it doesn't own the CellApp state, it
// reaches into it through a caller-supplied lookup. That lets tests and
// the real CellApp process share the same provider class without the
// provider having to know about Space, the tick loop, or BaseApp-bound
// routing.
//
// Step 10.7 stands up the skeleton; Step 10.8's CellApp process is what
// actually constructs this class, wires the lookup into its
// base_entity_population_ map, and installs the provider via
// SetNativeApiProvider().
// ============================================================================

class CellAppNativeProvider : public BaseNativeProvider {
 public:
  // Resolve a CellEntity* from a C# entity id. Return nullptr if the
  // id doesn't correspond to a live cell entity — the provider's methods
  // log+skip in that case so a misbehaving script can't crash the
  // process. In practice, the C# runtime identifies entities by their
  // cell_entity_id (see phase10_cellapp.md §9.6).
  using EntityLookupFn = std::function<CellEntity*(uint32_t entity_id)>;

  // `network` is used by SendClientRpc to open a RUDP connection back to
  // the owning BaseApp. For handler-level tests that never exercise that
  // path, the default ctor with just the lookup is still available.
  explicit CellAppNativeProvider(EntityLookupFn lookup);
  CellAppNativeProvider(EntityLookupFn lookup, NetworkInterface& network);

  uint8_t GetProcessPrefix() override;

  // Cell → owning-client RPC. Wraps into baseapp::SelfRpcFromCell and
  // sends to the entity's base_addr. BaseApp's OnSelfRpcFromCell relays
  // to the client on the RUDP channel using rpc_id as the wire msg_id.
  void SendClientRpc(uint32_t entity_id, uint32_t rpc_id, const std::byte* payload,
                     int32_t len) override;

  // Phase 10 CellApp-specific surfaces.
  void SetEntityPosition(uint32_t entity_id, float x, float y, float z) override;
  void PublishReplicationFrame(uint32_t entity_id, uint64_t event_seq, uint64_t volatile_seq,
                               const std::byte* owner_snap, int32_t owner_snap_len,
                               const std::byte* other_snap, int32_t other_snap_len,
                               const std::byte* owner_delta, int32_t owner_delta_len,
                               const std::byte* other_delta, int32_t other_delta_len) override;
  auto AddMoveController(uint32_t entity_id, float dest_x, float dest_y, float dest_z, float speed,
                         int32_t user_arg) -> int32_t override;
  auto AddTimerController(uint32_t entity_id, float interval, bool repeat, int32_t user_arg)
      -> int32_t override;
  auto AddProximityController(uint32_t entity_id, float range, int32_t user_arg)
      -> int32_t override;
  void CancelController(uint32_t entity_id, int32_t controller_id) override;

  // ---- C# → C++ callback table ------------------------------------------
  void SetNativeCallbacks(const void* native_callbacks, int32_t len) override;

  // ---- Callback accessors (used by CellApp message handlers) -------------
  [[nodiscard]] auto restore_entity_fn() const -> RestoreEntityFn { return restore_entity_fn_; }
  [[nodiscard]] auto dispatch_rpc_fn() const -> DispatchRpcFn { return dispatch_rpc_fn_; }
  [[nodiscard]] auto entity_destroyed_fn() const -> EntityDestroyedFn {
    return entity_destroyed_fn_;
  }
  [[nodiscard]] auto serialize_entity_fn() const -> SerializeEntityFn {
    return serialize_entity_fn_;
  }

 private:
  EntityLookupFn lookup_;
  NetworkInterface* network_{nullptr};  // null for handler-level tests
  RestoreEntityFn restore_entity_fn_{nullptr};
  DispatchRpcFn dispatch_rpc_fn_{nullptr};
  EntityDestroyedFn entity_destroyed_fn_{nullptr};
  // Phase 11 PR-6: used by CellApp::BuildOffloadMessage to capture the
  // outgoing Real entity's full state. nullptr until C# registers the
  // expanded NativeCallbackTable; absence is not fatal (CellApp ships an
  // empty persistent_blob and the receiver proceeds using only the
  // replication baseline, as in PR-4).
  SerializeEntityFn serialize_entity_fn_{nullptr};
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_CELLAPP_NATIVE_PROVIDER_H_
