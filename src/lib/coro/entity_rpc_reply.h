#ifndef ATLAS_LIB_CORO_ENTITY_RPC_REPLY_H_
#define ATLAS_LIB_CORO_ENTITY_RPC_REPLY_H_

#include <cstdint>
#include <span>
#include <string_view>

#include "foundation/error.h"
#include "network/channel.h"
#include "network/message.h"
#include "network/message_ids.h"
#include "serialization/binary_stream.h"

namespace atlas {

// Send-only NetworkMessage carrying an entity-RPC reply. body is opaque:
// for success it is the deserializer-readable reply payload; for failure
// it is a VLE-prefixed error message string. The C# AtlasRpcSource<T>
// matching this reply consumes [request_id][error_code] header itself.
struct EntityRpcReply {
  uint32_t request_id{0};
  int32_t error_code{0};
  std::span<const std::byte> body;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Common::kEntityRpcReply),
                                   "Common::EntityRpcReply",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(request_id);
    w.Write(error_code);
    w.WriteBytes(body);
  }

  // Send-only — replies are claimed by PendingRpcRegistry via the wire-level
  // pre-dispatch hook, never typed-dispatched.
  static auto Deserialize(BinaryReader&) -> Result<EntityRpcReply> {
    return Error{ErrorCode::kInvalidArgument, "EntityRpcReply is send-only"};
  }
};
static_assert(NetworkMessage<EntityRpcReply>);

namespace entity_rpc_reply {

// Builds the body for an error reply: [vle-string error_msg].
inline void WriteErrorBody(BinaryWriter& body, std::string_view error_msg) {
  body.WriteString(error_msg);
}

inline auto SendSuccess(Channel& ch, uint32_t request_id, std::span<const std::byte> body)
    -> Result<void> {
  return ch.SendMessage(EntityRpcReply{
      .request_id = request_id,
      .error_code = 0,
      .body = body,
  });
}

inline auto SendFailure(Channel& ch, uint32_t request_id, int32_t error_code,
                        std::string_view error_msg) -> Result<void> {
  BinaryWriter body;
  WriteErrorBody(body, error_msg);
  return ch.SendMessage(EntityRpcReply{
      .request_id = request_id,
      .error_code = error_code,
      .body = body.Data(),
  });
}

}  // namespace entity_rpc_reply

}  // namespace atlas

#endif  // ATLAS_LIB_CORO_ENTITY_RPC_REPLY_H_
