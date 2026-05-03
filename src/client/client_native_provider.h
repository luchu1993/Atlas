#ifndef ATLAS_CLIENT_CLIENT_NATIVE_PROVIDER_H_
#define ATLAS_CLIENT_CLIENT_NATIVE_PROVIDER_H_

#include <cstdint>

#include "clrscript/base_native_provider.h"

namespace atlas {

class ClientApp;

// ============================================================================
// DispatchClientRpcFn — C++ calls into C# when a ClientRpc arrives from server
// ============================================================================

using ClientDispatchRpcFn = void (*)(uint32_t entity_id, uint32_t rpc_id, const uint8_t* payload,
                                     int32_t len, uint64_t trace_id);
using ClientCreateEntityFn = void (*)(uint32_t entity_id, uint16_t type_id);
using ClientDestroyEntityFn = void (*)(uint32_t entity_id);

// Opaque transport-channel delivery hook. C++ routes a reserved client-facing
// MessageID (0xF001 unreliable delta / 0xF002 baseline / 0xF003 reliable
// delta) to the script host without decoding the payload — the envelope kind,
// entity id, and field bytes are all parsed on the managed side. This keeps
// the native provider free of property-sync business logic so a future Lua
// or TypeScript host can bind the same transport hook unchanged.
using ClientDeliverFromServerFn = void (*)(uint16_t msg_id, const uint8_t* payload, int32_t len);

// ============================================================================
// ClientNativeProvider — INativeApiProvider for the client process
// ============================================================================

class ClientNativeProvider : public BaseNativeProvider {
 public:
  explicit ClientNativeProvider(ClientApp& app);

  uint8_t GetProcessPrefix() override;

  // Client sends exposed RPCs to server via BaseApp
  void SendBaseRpc(uint32_t entity_id, uint32_t rpc_id, const std::byte* payload, int32_t len,
                   uint64_t trace_id) override;
  void SendCellRpc(uint32_t entity_id, uint32_t rpc_id, const std::byte* payload, int32_t len,
                   uint64_t trace_id) override;

  // Client telemetry: periodic gap-count report to BaseApp.
  void ReportClientEventSeqGap(uint32_t entity_id, uint32_t gap_delta) override;

  // C# callback table registration
  void SetNativeCallbacks(const void* native_callbacks, int32_t len) override;

  // Callback accessors
  [[nodiscard]] auto DispatchRpcFn() const -> ClientDispatchRpcFn { return dispatch_rpc_fn_; }
  [[nodiscard]] auto CreateEntityFn() const -> ClientCreateEntityFn { return create_entity_fn_; }
  [[nodiscard]] auto DestroyEntityFn() const -> ClientDestroyEntityFn { return destroy_entity_fn_; }
  [[nodiscard]] auto DeliverFromServerFn() const -> ClientDeliverFromServerFn {
    return deliver_from_server_fn_;
  }

 private:
  ClientApp& app_;
  ClientDispatchRpcFn dispatch_rpc_fn_{nullptr};
  ClientCreateEntityFn create_entity_fn_{nullptr};
  ClientDestroyEntityFn destroy_entity_fn_{nullptr};
  ClientDeliverFromServerFn deliver_from_server_fn_{nullptr};
};

}  // namespace atlas

#endif  // ATLAS_CLIENT_CLIENT_NATIVE_PROVIDER_H_
