#ifndef ATLAS_LIB_NETWORK_CHANNEL_H_
#define ATLAS_LIB_NETWORK_CHANNEL_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "foundation/clock.h"
#include "foundation/timer_queue.h"
#include "network/address.h"
#include "network/bundle.h"
#include "network/packet_filter.h"
#include "platform/io_poller.h"

// Windows <winuser.h> defines SendMessage as a macro.
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

// Not thread-safe; used from EventDispatcher's thread only.
class Channel {
 public:
  Channel(EventDispatcher& dispatcher, InterfaceTable& table, const Address& remote);
  virtual ~Channel();

  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;
  Channel(Channel&&) = delete;
  Channel& operator=(Channel&&) = delete;

  [[nodiscard]] auto Bundle() -> Bundle& { return bundle_; }
  [[nodiscard]] auto Send() -> Result<void>;

  // Message descriptors select reliability and batching policy.
  template <NetworkMessage Msg>
  [[nodiscard]] auto SendMessage(const Msg& msg) -> Result<void> {
    if (IsCondemned()) return CondemnedSendError();
    const auto& desc = Msg::Descriptor();
    if (desc.IsBatched()) {
      if (auto* deferred = DeferredBundleFor(desc)) {
        deferred->AddMessage(msg);
        return OnDeferredAppend(desc, deferred->TotalSize());
      }
    }
    bundle_.AddMessage(msg);
    if (desc.IsUnreliable()) return SendUnreliable();
    return Send();
  }

  [[nodiscard]] virtual auto SendUnreliable() -> Result<void>;

  // Drains messages staged through the kBatched path.
  [[nodiscard]] virtual auto FlushDeferred() -> Result<void> { return Result<void>{}; }

  using MarkDirtyCallback = std::function<void(Channel&)>;
  void SetMarkDirtyCallback(MarkDirtyCallback cb) { mark_dirty_cb_ = std::move(cb); }

  [[nodiscard]] auto State() const -> ChannelState { return state_; }
  [[nodiscard]] auto ChannelId() const -> ChannelId { return channel_id_; }
  [[nodiscard]] auto RemoteAddress() const -> const Address& { return remote_; }
  [[nodiscard]] auto IsConnected() const -> bool { return state_ == ChannelState::kActive; }
  [[nodiscard]] auto IsCondemned() const -> bool { return state_ == ChannelState::kCondemned; }

  // Condemned RUDP channels may need to outlive active map removal.
  [[nodiscard]] virtual auto HasUnackedReliablePackets() const -> bool { return false; }
  [[nodiscard]] virtual auto HasRemoteFailed() const -> bool { return false; }

  void Activate();
  void Condemn();

  void SetInactivityTimeout(Duration timeout);
  void ResetInactivityTimer();

  [[nodiscard]] auto BytesSent() const -> uint64_t { return bytes_sent_; }
  [[nodiscard]] auto BytesReceived() const -> uint64_t { return bytes_received_; }

  void SetPacketFilter(PacketFilterPtr filter) { packet_filter_ = std::move(filter); }
  [[nodiscard]] auto PacketFilter() const -> PacketFilter* { return packet_filter_.get(); }

  using DisconnectCallback = std::function<void(Channel&)>;
  void SetDisconnectCallback(DisconnectCallback cb);
  void SetChannelId(::atlas::ChannelId id);

  [[nodiscard]] virtual auto Fd() const -> FdHandle = 0;

 protected:
  [[nodiscard]] auto CondemnedSendError() const -> Result<void>;

  void NotifyDirty() {
    if (mark_dirty_cb_) mark_dirty_cb_(*this);
  }

  // nullptr keeps kBatched messages on the immediate send path.
  [[nodiscard]] virtual auto DeferredBundleFor(const MessageDesc& /*desc*/) -> class Bundle* {
    return nullptr;
  }
  [[nodiscard]] virtual auto OnDeferredAppend(const MessageDesc& /*desc*/,
                                              std::size_t /*total_size*/) -> Result<void> {
    return Result<void>{};
  }

  [[nodiscard]] virtual auto DoSend(std::span<const std::byte> data) -> Result<size_t> = 0;
  virtual void OnCondemned() {}
  void OnDataReceived(std::span<const std::byte> data);
  void OnDisconnect();

  void DispatchMessages(std::span<const std::byte> frame_data);

  // Returns true when dispatch yielded and left a tail for later.
  [[nodiscard]] auto DispatchMessagesBudgeted(std::span<const std::byte> frame_data,
                                              TimePoint deadline) -> bool;

  [[nodiscard]] auto DrainPendingDispatch(TimePoint deadline) -> bool;

 public:
  [[nodiscard]] auto HasPendingDispatch() const -> bool {
    return pending_dispatch_pos_ < pending_dispatch_buf_.size();
  }

 protected:
  EventDispatcher& dispatcher_;
  InterfaceTable& interface_table_;
  Address remote_;
  ChannelState state_{ChannelState::kCreated};
  class Bundle bundle_;
  ::atlas::ChannelId channel_id_{kInvalidChannelId};

  std::vector<std::byte> pending_dispatch_buf_;
  std::size_t pending_dispatch_pos_{0};

  uint64_t bytes_sent_{0};
  uint64_t bytes_received_{0};

  PacketFilterPtr packet_filter_;
  DisconnectCallback disconnect_callback_;
  MarkDirtyCallback mark_dirty_cb_;
  Duration inactivity_timeout_{};
  TimerHandle inactivity_timer_;
  TimePoint last_activity_{};

 private:
  void CheckInactivity();
  void StartInactivityTimer();
};

}  // namespace atlas

#endif  // ATLAS_LIB_NETWORK_CHANNEL_H_
