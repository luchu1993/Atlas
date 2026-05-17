#ifndef ATLAS_UE_CLIENT_CORE_RPC_SENDER_H_
#define ATLAS_UE_CLIENT_CORE_RPC_SENDER_H_

#include <cstdint>

#include "AtlasCore/entity_id.h"

namespace atlas {

// Engine-side outbound RPC bridge. Codegen-emitted entity classes resolve
// `Sender()` at call time so generated stubs stay free of UE / transport
// dependencies; the actual wire send happens through the implementation
// (UAtlasSubsystem on UE, an in-process loopback in tests).
class RpcSender {
 public:
  virtual ~RpcSender() = default;

  virtual void SendBaseRpc(EntityId id, uint32_t rpc_id, const uint8_t* args,
                            int32_t args_len) = 0;

  virtual void SendCellRpc(EntityId id, uint32_t rpc_id, const uint8_t* args,
                            int32_t args_len) = 0;
};

}  // namespace atlas

#endif  // ATLAS_UE_CLIENT_CORE_RPC_SENDER_H_
