#ifndef ATLAS_LIB_NETWORK_CHANNEL_H_
#define ATLAS_LIB_NETWORK_CHANNEL_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

#include "foundation/clock.h"
#include "foundation/timer_queue.h"
#include "network/address.h"
#include "network/bundle.h"
#include "network/packet_filter.h"
#include "platform/io_poller.h"

// Windows <winuser.h> defines SendMessage as a macro, which collides with
// Channel::SendMessage.  Undefine it so our method name compiles cleanly.
#ifdef SendMessage
#undef SendMessage
#endif

namespace atlas {

class EventDispatcher;
class InterfaceTable;
using ChannelId = uint64_t;
inline constexpr ChannelId kInvalidChannelId = 0;

enum class ChannelState : uint8_t {
  kCreated,
  kActive,
  kCondemned,
};

// Thread safety: NOT thread-safe. Used from EventDispatcher's thread only.
class Channel {
 public:
  Channel(EventDispatcher& dispatcher, InterfaceTable& table, const Address& remote);
  virtual ~Channel();

  // Non-copyable, non-movable
  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;
  Channel(Channel&&) = delete;
  Channel& operator=(Channel&&) = delete;

  // Sending
  [[nodiscard]] auto Bundle() -> Bundle& { return bundle_; }
  [[nodiscard]] auto Send() -> Result<void>;
  // Both overloads honour Msg::descriptor().reliability (or the registered
  // InterfaceTable entry for the raw-ID variant):
  //   Reliable   → send()            (ACK + retransmit on RUDP, always on TCP)
  //   Unreliable → send_unreliable() (best-effort on RUDP, natural path on TCP/UDP)
  [[nodiscard]] auto SendMessage(MessageID id, std::span<const std::byte> data) -> Result<void>;

  template <NetworkMessage Msg>
  void QueueMessage(const Msg& msg) {
    bundle_.AddMessage(msg);
  }

  template <NetworkMessage Msg>
  [[nodiscard]] auto SendMessage(const Msg& msg) -> Result<void> {
    bundle_.AddMessage(msg);
    if (Msg::Descriptor().IsUnreliable()) return SendUnreliable();
    return Send();
  }

  // Best-effort send — subclasses override to use unreliable path when available.
  // Default implementation falls back to send() (TCP is always reliable).
  [[nodiscard]] virtual auto SendUnreliable() -> Result<void>;

  // State
  [[nodiscard]] auto State() const -> ChannelState { return state_; }
  [[nodiscard]] auto ChannelId() const -> ChannelId { return channel_id_; }
  [[nodiscard]] auto RemoteAddress() const -> const Address& { return remote_; }
  [[nodiscard]] auto IsConnected() const -> bool { return state_ == ChannelState::kActive; }

  void Activate();
  void Condemn();

  // Inactivity detection
  void SetInactivityTimeout(Duration timeout);
  void ResetInactivityTimer();

  // Statistics
  [[nodiscard]] auto BytesSent() const -> uint64_t { return bytes_sent_; }
  [[nodiscard]] auto BytesReceived() const -> uint64_t { return bytes_received_; }

  // Packet filter (compression, encryption, etc.)
  void SetPacketFilter(PacketFilterPtr filter) { packet_filter_ = std::move(filter); }
  [[nodiscard]] auto PacketFilter() const -> PacketFilter* { return packet_filter_.get(); }

  // Disconnect callback
  using DisconnectCallback = std::function<void(Channel&)>;
  void SetDisconnectCallback(DisconnectCallback cb);
  void SetChannelId(::atlas::ChannelId id);

  // Access underlying fd for IOPoller registration
  [[nodiscard]] virtual auto Fd() const -> FdHandle = 0;

 protected:
  // Subclass hooks
  [[nodiscard]] virtual auto DoSend(std::span<const std::byte> data) -> Result<size_t> = 0;
  virtual void OnCondemned() {}
  void OnDataReceived(std::span<const std::byte> data);
  void OnDisconnect();

  // Message parsing helper: parse messages from a complete frame
  void DispatchMessages(std::span<const std::byte> frame_data);

  EventDispatcher& dispatcher_;
  InterfaceTable& interface_table_;
  Address remote_;
  ChannelState state_{ChannelState::kCreated};
  class Bundle bundle_;
  ::atlas::ChannelId channel_id_{kInvalidChannelId};

  uint64_t bytes_sent_{0};
  uint64_t bytes_received_{0};

  PacketFilterPtr packet_filter_;
  DisconnectCallback disconnect_callback_;
  Duration inactivity_timeout_{};
  TimerHandle inactivity_timer_;
  TimePoint last_activity_{};

 private:
  void CheckInactivity();
  void StartInactivityTimer();
};

}  // namespace atlas

#endif  // ATLAS_LIB_NETWORK_CHANNEL_H_
