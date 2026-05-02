#ifndef ATLAS_LIB_CLRSCRIPT_CORO_BRIDGE_H_
#define ATLAS_LIB_CLRSCRIPT_CORO_BRIDGE_H_

#include <cstddef>
#include <cstdint>

#include "coro/pending_rpc_registry.h"

namespace atlas {

// status values: 0=Success 1=Timeout 2=Cancelled 3=SendError. Mirrors
// Atlas.Coro.Rpc.RpcCompletionStatus.
using CoroOnRpcCompleteFn = void (*)(intptr_t managed_handle, int32_t status,
                                     const uint8_t* payload, int32_t len);

namespace coro_bridge {

// Handle encodes (reply_id, request_id); pass back to CancelPending.
// timeout_ms <= 0 means "never time out".
auto RegisterPending(PendingRpcRegistry& registry, CoroOnRpcCompleteFn on_complete,
                     uint16_t reply_id, uint32_t request_id, int32_t timeout_ms,
                     intptr_t managed_handle) -> uint64_t;

void CancelPending(PendingRpcRegistry& registry, uint64_t handle);

}  // namespace coro_bridge

}  // namespace atlas

#endif  // ATLAS_LIB_CLRSCRIPT_CORO_BRIDGE_H_
