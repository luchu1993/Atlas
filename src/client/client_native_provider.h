#ifndef ATLAS_CLIENT_CLIENT_NATIVE_PROVIDER_H_
#define ATLAS_CLIENT_CLIENT_NATIVE_PROVIDER_H_

#include <cstdint>

#include "clrscript/base_native_provider.h"

namespace atlas {

class ClientApp;

using ClientDispatchRpcFn = void (*)(uint32_t entity_id, uint32_t rpc_id, const uint8_t* payload,
                                     int32_t len, uint64_t trace_id);
using ClientCreateEntityFn = void (*)(uint32_t entity_id, uint16_t type_id);
using ClientDestroyEntityFn = void (*)(uint32_t entity_id);

// Opaque transport-channel hook for the 0xF001/0xF002/0xF003 wire ids — keeps
// property-sync decode on the managed side so a Lua / TS host can bind it too.
using ClientDeliverFromServerFn = void (*)(uint16_t msg_id, const uint8_t* payload, int32_t len);

class ClientNativeProvider : public BaseNativeProvider {
 public:
  explicit ClientNativeProvider(ClientApp& app);

  uint8_t GetProcessPrefix() override;

  void SendBaseRpc(uint32_t entity_id, uint32_t rpc_id, const std::byte* payload, int32_t len,
                   uint64_t trace_id) override;
  void SendCellRpc(uint32_t entity_id, uint32_t rpc_id, const std::byte* payload, int32_t len,
                   uint64_t trace_id) override;

  void ReportClientEventSeqGap(uint32_t entity_id, uint32_t gap_delta) override;

  void SetNativeCallbacks(const void* native_callbacks, int32_t len) override;

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
