#ifndef ATLAS_LIB_NETWORK_MESSAGE_H_
#define ATLAS_LIB_NETWORK_MESSAGE_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

#include "foundation/error.h"
#include "foundation/log.h"
#include "network/address.h"
#include "serialization/binary_stream.h"

namespace atlas {

using MessageID = uint16_t;

enum class MessageLengthStyle : uint8_t {
  kFixed,     // fixed-size payload, no length prefix
  kVariable,  // packed-int length prefix before payload
};

// Reliability hint embedded in the message descriptor.
//
// Reliable   — the channel must guarantee delivery and ordering (default).
//              On ReliableUdpChannel this uses the ACK/retransmit path.
//              On TcpChannel it is always reliable regardless of this flag.
//
// Unreliable — delivery is best-effort; lost packets are NOT retransmitted.
//              Use for high-frequency state updates (position, AoI deltas)
//              where staleness is worse than loss.
//              On non-RUDP channels (TCP, plain UDP) the channel falls back
//              to its natural send path (TCP is always reliable; plain UDP
//              never retransmits).
enum class MessageReliability : uint8_t {
  kReliable,
  kUnreliable,
};

// Urgency controls whether SendMessage on a batching-capable channel
// (RUDP) appends to a per-channel deferred bundle (kBatched) or
// flushes immediately (kImmediate).  Independent of reliability —
// every (reliability × urgency) combination is meaningful.
//
// Default is kImmediate so adding the field is behaviour-preserving;
// the descriptor audit to flip the default lands in a separate commit.
//
// kImmediate uses cases:
//   - Handshake / login / channel teardown
//   - PvP-critical command paths (combat actions, hit confirms)
//   - Anything where the caller awaits the syscall result
// kBatched use cases:
//   - State replication, RPC results, inventory updates
//   - Volatile position deltas (already covered by witness path today)
enum class MessageUrgency : uint8_t {
  kImmediate,
  kBatched,
};

struct MessageDesc {
  MessageID id;
  std::string_view name;
  MessageLengthStyle length_style;
  int32_t fixed_length;                                           // bytes, only for Fixed style
  MessageReliability reliability{MessageReliability::kReliable};  // delivery guarantee
  MessageUrgency urgency{MessageUrgency::kImmediate};             // batching opt-in

  [[nodiscard]] constexpr auto IsFixed() const -> bool {
    return length_style == MessageLengthStyle::kFixed;
  }

  [[nodiscard]] constexpr auto IsUnreliable() const -> bool {
    return reliability == MessageReliability::kUnreliable;
  }

  [[nodiscard]] constexpr auto IsBatched() const -> bool {
    return urgency == MessageUrgency::kBatched;
  }
};

// Forward declare Channel
class Channel;

// Handler interface
class MessageHandler {
 public:
  virtual ~MessageHandler() = default;
  virtual void HandleMessage(const Address& source, Channel* channel, MessageID id,
                             BinaryReader& data) = 0;
};

// C++20 concept: a type is a NetworkMessage if it provides these
template <typename T>
concept NetworkMessage = requires(const T& msg, BinaryWriter& w, BinaryReader& r) {
  { T::Descriptor() } -> std::same_as<const MessageDesc&>;
  { msg.Serialize(w) } -> std::same_as<void>;
  { T::Deserialize(r) } -> std::same_as<Result<T>>;
};

// Typed handler that auto-deserializes and calls a callback
template <NetworkMessage Msg>
class TypedMessageHandler : public MessageHandler {
 public:
  using Callback = std::function<void(const Address&, Channel*, const Msg&)>;

  explicit TypedMessageHandler(Callback callback) : callback_(std::move(callback)) {}

  void HandleMessage(const Address& source, Channel* channel, MessageID id,
                     BinaryReader& data) override {
    auto result = Msg::Deserialize(data);
    if (result.HasValue()) {
      callback_(source, channel, result.Value());
    } else {
      ATLAS_LOG_WARNING("Failed to deserialize message {} from {}", id, source.ToString());
    }
  }

 private:
  Callback callback_;
};

// Factory for typed handlers
template <NetworkMessage Msg>
[[nodiscard]] auto MakeHandler(typename TypedMessageHandler<Msg>::Callback callback)
    -> std::unique_ptr<MessageHandler> {
  return std::make_unique<TypedMessageHandler<Msg>>(std::move(callback));
}

}  // namespace atlas

#endif  // ATLAS_LIB_NETWORK_MESSAGE_H_
