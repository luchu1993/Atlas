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

  // Single public send entry point. Descriptor's reliability picks
  // Send / SendUnreliable; urgency picks deferred batching vs immediate.
  template <NetworkMessage Msg>
  [[nodiscard]] auto SendMessage(const Msg& msg) -> Result<void> {
    if (IsCondemned()) return CondemnedSendError();
    const auto& desc = Msg::Descriptor();
    if (desc.IsBatched()) {
      if (auto* deferred = DeferredBundleFor(desc)) {
        deferred->AddMessage(msg);
        return OnDeferredAppend(desc, deferred->TotalSize());
      }
      // No batching available — fall through to immediate.
    }
    bundle_.AddMessage(msg);
    if (desc.IsUnreliable()) return SendUnreliable();
    return Send();
  }

  // TCP defaults to Send (always reliable); UDP/RUDP override.
  [[nodiscard]] virtual auto SendUnreliable() -> Result<void>;

  // Drain messages staged via the kBatched path. Invoked by
  // NetworkInterface::FlushDirtySendChannels at I/O readable tail and
  // ServerApp::FlushTickDirtyChannels at tick close.
  [[nodiscard]] virtual auto FlushDeferred() -> Result<void> { return Result<void>{}; }

  // Optional — channels created outside NetworkInterface (tests) leave
  // it unset and call FlushDeferred explicitly.
  using MarkDirtyCallback = std::function<void(Channel&)>;
  void SetMarkDirtyCallback(MarkDirtyCallback cb) { mark_dirty_cb_ = std::move(cb); }

  [[nodiscard]] auto State() const -> ChannelState { return state_; }
  [[nodiscard]] auto ChannelId() const -> ChannelId { return channel_id_; }
  [[nodiscard]] auto RemoteAddress() const -> const Address& { return remote_; }
  [[nodiscard]] auto IsConnected() const -> bool { return state_ == ChannelState::kActive; }
  [[nodiscard]] auto IsCondemned() const -> bool { return state_ == ChannelState::kCondemned; }

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
  // Logs at DEBUG so silent (void)-ignored sites still surface.
  [[nodiscard]] auto CondemnedSendError() const -> Result<void>;

  void NotifyDirty() {
    if (mark_dirty_cb_) mark_dirty_cb_(*this);
  }

  // Subclasses with per-channel deferred bundles (ReliableUdpChannel)
  // override DeferredBundleFor; others return nullptr and SendMessage
  // takes the immediate path. OnDeferredAppend marks dirty + enforces
  // a size threshold; threshold-flush errors propagate back via Result.
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

  // Per-handler-deadline variant. Unprocessed tail parks in
  // pending_dispatch_buf_; caller drains via DrainPendingDispatch.
  // Returns true iff frame_data was not fully consumed OR pending is
  // non-empty after the call.
  [[nodiscard]] auto DispatchMessagesBudgeted(std::span<const std::byte> frame_data,
                                              TimePoint deadline) -> bool;

  // Returns true iff pending is still non-empty.
  [[nodiscard]] auto DrainPendingDispatch(TimePoint deadline) -> bool;

  // Public so ReliableUdpChannel::HasReceiveBacklog can include
  // pending-dispatch alongside rcv_buf_.
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
