#ifndef ATLAS_LIB_SERVER_COMMON_MESSAGES_H_
#define ATLAS_LIB_SERVER_COMMON_MESSAGES_H_

#include <cstdint>

#include "network/message.h"
#include "network/message_ids.h"
#include "serialization/binary_stream.h"

// ============================================================================
// Common messages shared by all Atlas server processes
//
// Message ID allocation (see network/message_ids.hpp for the authoritative
// registry — do NOT add numeric IDs here or in individual message headers):
//   0     –    99   reserved
//   100   –   199   common      (this file)
//   1000  –  1099   machined    (machined_types.hpp)
//   2000  –  2999   BaseApp     (baseapp_messages.hpp)
//   3000  –  3999   CellApp     — reserved
//   4000  –  4999   DBApp       (dbapp_messages.hpp)
//   5000  –  5999   LoginApp    (login_messages.hpp)
//   6000  –  6999   BaseAppMgr  (baseappmgr_messages.hpp)
//   7000  –  7999   CellAppMgr  — reserved
//   8000  –  8999   DBAppMgr    — reserved
//   10000 – 19999   external (client ↔ server)
//   50000 – 59999   C# RPC forwarding
// ============================================================================

namespace atlas::msg {

// ---- Heartbeat (all processes → machined) -----------------------------------

struct Heartbeat {
  uint64_t game_time{0};  // current game tick count
  float load{0.0f};       // normalised load 0.0–1.0

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc desc{msg_id::Id(msg_id::Common::kHeartbeat), "Heartbeat",
                                  MessageLengthStyle::kFixed,
                                  static_cast<int32_t>(sizeof(uint64_t) + sizeof(float))};
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

// ---- ShutdownRequest (Manager → processes) ----------------------------------

struct ShutdownRequest {
  uint8_t reason{0};  // 0=normal, 1=maintenance, 2=emergency

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc desc{msg_id::Id(msg_id::Common::kShutdownRequest), "ShutdownRequest",
                                  MessageLengthStyle::kFixed,
                                  static_cast<int32_t>(sizeof(uint8_t))};
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
