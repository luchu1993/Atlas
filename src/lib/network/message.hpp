#pragma once

#include "foundation/error.hpp"
#include "network/address.hpp"
#include "serialization/binary_stream.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace atlas
{

using MessageID = uint16_t;

enum class MessageLengthStyle : uint8_t
{
    Fixed,     // fixed-size payload, no length prefix
    Variable,  // packed-int length prefix before payload
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
enum class MessageReliability : uint8_t
{
    Reliable,
    Unreliable,
};

struct MessageDesc
{
    MessageID id;
    std::string_view name;
    MessageLengthStyle length_style;
    int32_t fixed_length;                                          // bytes, only for Fixed style
    MessageReliability reliability{MessageReliability::Reliable};  // delivery guarantee

    [[nodiscard]] constexpr auto is_fixed() const -> bool
    {
        return length_style == MessageLengthStyle::Fixed;
    }

    [[nodiscard]] constexpr auto is_unreliable() const -> bool
    {
        return reliability == MessageReliability::Unreliable;
    }
};

// Forward declare Channel
class Channel;

// Handler interface
class MessageHandler
{
public:
    virtual ~MessageHandler() = default;
    virtual void handle_message(const Address& source, Channel* channel, MessageID id,
                                BinaryReader& data) = 0;
};

// C++20 concept: a type is a NetworkMessage if it provides these
template <typename T>
concept NetworkMessage = requires(const T& msg, BinaryWriter& w, BinaryReader& r) {
    { T::descriptor() } -> std::same_as<const MessageDesc&>;
    { msg.serialize(w) } -> std::same_as<void>;
    { T::deserialize(r) } -> std::same_as<Result<T>>;
};

// Typed handler that auto-deserializes and calls a callback
template <NetworkMessage Msg>
class TypedMessageHandler : public MessageHandler
{
public:
    using Callback = std::function<void(const Address&, Channel*, const Msg&)>;

    explicit TypedMessageHandler(Callback callback) : callback_(std::move(callback)) {}

    void handle_message(const Address& source, Channel* channel, MessageID id,
                        BinaryReader& data) override
    {
        auto result = Msg::deserialize(data);
        if (result.has_value())
        {
            callback_(source, channel, result.value());
        }
        // If deserialization fails, silently discard (logged at dispatch level)
    }

private:
    Callback callback_;
};

// Factory for typed handlers
template <NetworkMessage Msg>
[[nodiscard]] auto make_handler(typename TypedMessageHandler<Msg>::Callback callback)
    -> std::unique_ptr<MessageHandler>
{
    return std::make_unique<TypedMessageHandler<Msg>>(std::move(callback));
}

}  // namespace atlas
