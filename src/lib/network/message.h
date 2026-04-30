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

// Transport hint; TCP remains reliable regardless of this value.
enum class MessageReliability : uint8_t {
  kReliable,
  kUnreliable,
};

// Batching hint; descriptors default to kBatched unless latency is part of
// the protocol contract.
enum class MessageUrgency : uint8_t {
  kImmediate,
  kBatched,
};

struct MessageDesc {
  MessageID id;
  std::string_view name;
  MessageLengthStyle length_style;
  int32_t fixed_length;
  MessageReliability reliability{MessageReliability::kReliable};
  MessageUrgency urgency{MessageUrgency::kBatched};

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

class Channel;

class MessageHandler {
 public:
  virtual ~MessageHandler() = default;
  virtual void HandleMessage(const Address& source, Channel* channel, MessageID id,
                             BinaryReader& data) = 0;
};

template <typename T>
concept NetworkMessage = requires(const T& msg, BinaryWriter& w, BinaryReader& r) {
  { T::Descriptor() } -> std::same_as<const MessageDesc&>;
  { msg.Serialize(w) } -> std::same_as<void>;
  { T::Deserialize(r) } -> std::same_as<Result<T>>;
};

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

template <NetworkMessage Msg>
[[nodiscard]] auto MakeHandler(typename TypedMessageHandler<Msg>::Callback callback)
    -> std::unique_ptr<MessageHandler> {
  return std::make_unique<TypedMessageHandler<Msg>>(std::move(callback));
}

}  // namespace atlas

#endif  // ATLAS_LIB_NETWORK_MESSAGE_H_
