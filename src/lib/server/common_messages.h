#ifndef ATLAS_LIB_SERVER_COMMON_MESSAGES_H_
#define ATLAS_LIB_SERVER_COMMON_MESSAGES_H_

#include <cstdint>

#include "network/message.h"
#include "network/message_ids.h"
#include "serialization/binary_stream.h"

namespace atlas::msg {

struct Heartbeat {
  uint64_t game_time{0};  // current game tick count
  float load{0.0f};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc desc{msg_id::Id(msg_id::Common::kHeartbeat),
                                  "Heartbeat",
                                  MessageLengthStyle::kFixed,
                                  static_cast<int32_t>(sizeof(uint64_t) + sizeof(float)),
                                  MessageReliability::kReliable,
                                  MessageUrgency::kImmediate};
    return desc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write<uint64_t>(game_time);
    w.Write<float>(load);
  }

  static auto Deserialize(BinaryReader& r) -> Result<Heartbeat> {
    auto gt = r.Read<uint64_t>();
    if (!gt) return gt.Error();
    auto ld = r.Read<float>();
    if (!ld) return ld.Error();
    return Heartbeat{*gt, *ld};
  }
};

static_assert(NetworkMessage<Heartbeat>);

struct ShutdownRequest {
  uint8_t reason{0};  // 0=normal, 1=maintenance, 2=emergency

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc desc{msg_id::Id(msg_id::Common::kShutdownRequest),
                                  "ShutdownRequest",
                                  MessageLengthStyle::kFixed,
                                  static_cast<int32_t>(sizeof(uint8_t)),
                                  MessageReliability::kReliable,
                                  MessageUrgency::kImmediate};
    return desc;
  }

  void Serialize(BinaryWriter& w) const { w.Write<uint8_t>(reason); }

  static auto Deserialize(BinaryReader& r) -> Result<ShutdownRequest> {
    auto reason = r.Read<uint8_t>();
    if (!reason) return reason.Error();
    return ShutdownRequest{*reason};
  }
};

static_assert(NetworkMessage<ShutdownRequest>);

}  // namespace atlas::msg

#endif  // ATLAS_LIB_SERVER_COMMON_MESSAGES_H_
