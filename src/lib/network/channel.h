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
  // Urgency is also consulted: kBatched routes through the deferred path
  // (RUDP only — TCP/UDP fall through to immediate send).
  [[nodiscard]] auto SendMessage(MessageID id, std::span<const std::byte> data) -> Result<void>;

  template <NetworkMessage Msg>
  void QueueMessage(const Msg& msg) {
    bundle_.AddMessage(msg);
  }

  template <NetworkMessage Msg>
  [[nodiscard]] auto SendMessage(const Msg& msg) -> Result<void> {
    if (IsCondemned()) return CondemnedSendError();
    const auto& desc = Msg::Descriptor();
    if (desc.IsBatched()) {
      if (auto* deferred = DeferredBundleFor(desc)) {
        // RUDP path: append directly to the per-channel deferred
        // bundle without going through scratch, so witness-volume
        // traffic stays single-copy.
        deferred->AddMessage(msg);
        return OnDeferredAppend(desc, deferred->TotalSize());
      }
      // Fallback (TCP / plain UDP / unit-test channels): no per-
      // channel batching exists — send immediately.
    }
    bundle_.AddMessage(msg);
    if (desc.IsUnreliable()) return SendUnreliable();
    return Send();
  }

  // Best-effort send — subclasses override to use unreliable path when available.
  // Default implementation falls back to send() (TCP is always reliable).
  [[nodiscard]] virtual auto SendUnreliable() -> Result<void>;

  // Drain any messages staged via the kBatched path (DeferredBundleFor +
  // OnDeferredAppend).  Default no-op: a channel that returns nullptr
  // from DeferredBundleFor never accumulates anything to flush.  RUDP
  // and TCP override; UDP keeps the default since each datagram is an
  // independent send and there is no application-level bundle to drain.
  // Called by NetworkInterface::FlushDirtySendChannels at the end of
  // every readable callback and by ServerApp::FlushTickDirtyChannels at
  // tick close — see docs/optimization/channel_send_batching.md.
  [[nodiscard]] virtual auto FlushDeferred() -> Result<void> { return Result<void>{}; }

  // Mark-dirty callback installed by NetworkInterface.  When a channel
  // appends to its deferred bundle, it invokes the callback so the
  // owning interface can register the channel for tick-end flush.  The
  // callback is optional — channels created outside NetworkInterface
  // (unit tests) leave it unset and are responsible for explicit
  // FlushDeferred calls.
  using MarkDirtyCallback = std::function<void(Channel&)>;
  void SetMarkDirtyCallback(MarkDirtyCallback cb) { mark_dirty_cb_ = std::move(cb); }

  // State
  [[nodiscard]] auto State() const -> ChannelState { return state_; }
  [[nodiscard]] auto ChannelId() const -> ChannelId { return channel_id_; }
  [[nodiscard]] auto RemoteAddress() const -> const Address& { return remote_; }
  [[nodiscard]] auto IsConnected() const -> bool { return state_ == ChannelState::kActive; }
  [[nodiscard]] auto IsCondemned() const -> bool { return state_ == ChannelState::kCondemned; }

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
  // Build the canonical "send on condemned channel" error.  Logs at
  // DEBUG so silent (void)-ignored sites surface without changing
  // call sites.  Used by both SendMessage overloads — every other
  // public send entry (Send / SendUnreliable / FlushDeferred) does
  // its own pre-existing condemned check + bundle Clear, since they
  // can be called outside SendMessage and own different bundles.
  [[nodiscard]] auto CondemnedSendError() const -> Result<void>;

  // Invoke mark_dirty_cb_ if installed.  Subclass batching overrides
  // call this after appending to a deferred bundle so NetworkInterface
  // schedules a tick-end flush.
  void NotifyDirty() {
    if (mark_dirty_cb_) mark_dirty_cb_(*this);
  }

  // Batching hooks.  A channel that supports per-channel deferred-send
  // bundles (ReliableUdpChannel) overrides DeferredBundleFor to return
  // the bundle matching the descriptor's reliability axis; channels
  // without batching benefit (TCP — kernel-level coalesce already; UDP
  // — no ordered framing) leave it returning nullptr and SendMessage
  // falls through to the immediate path.
  //
  // OnDeferredAppend is invoked by SendMessage right after writing
  // into the deferred bundle.  Subclasses use it to register dirty
  // and to enforce the size threshold; default no-op.  Returns
  // Result<void> so the size-threshold flush can surface its error
  // back to the SendMessage caller.
  [[nodiscard]] virtual auto DeferredBundleFor(const MessageDesc& /*desc*/) -> class Bundle* {
    return nullptr;
  }
  [[nodiscard]] virtual auto OnDeferredAppend(const MessageDesc& /*desc*/,
                                              std::size_t /*total_size*/) -> Result<void> {
    return Result<void>{};
  }

  // Subclass hooks
  [[nodiscard]] virtual auto DoSend(std::span<const std::byte> data) -> Result<size_t> = 0;
  virtual void OnCondemned() {}
  void OnDataReceived(std::span<const std::byte> data);
  void OnDisconnect();

  // Run dispatch on a fully-received frame, no time bound.  Used by
  // unreliable per-packet paths where there's no cascade concern, and
  // by the legacy synchronous wrapper that tests rely on.
  void DispatchMessages(std::span<const std::byte> frame_data);

  // Run dispatch with a per-handler deadline.  After every successful
  // handler call we check `deadline`; if it has passed, the unprocessed
  // tail of `frame_data` (everything after the last fully-dispatched
  // message) is appended to the channel's pending-dispatch buffer and
  // the function returns true.  The caller (typically
  // ReliableUdpChannel::FlushReceiveBuffer) drains the pending buffer
  // on its own cadence via DrainPendingDispatch.
  //
  // This bounds the dispatcher stall to one application handler's
  // worth of work (~25 ms at 500-cli load) instead of one whole MTU-
  // packet's worth (~166 ms with 10 bundled handlers).  See
  // docs/optimization/network_dispatch_decoupling.md for the data that
  // motivates this granularity.
  //
  // Returns true iff `frame_data` was not fully consumed AND/OR the
  // pending buffer is non-empty after the call.
  [[nodiscard]] auto DispatchMessagesBudgeted(std::span<const std::byte> frame_data,
                                              TimePoint deadline) -> bool;

  // Resume a previously-yielded dispatch.  Drains pending bytes up to
  // `deadline`.  Returns true iff pending is still non-empty.
  [[nodiscard]] auto DrainPendingDispatch(TimePoint deadline) -> bool;

  // Probe.  Public so ReliableUdpChannel::HasReceiveBacklog can include
  // pending-dispatch as well as rcv_buf_ when answering "is there work?"
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

  // A2 yield-buffer.  When DispatchMessagesBudgeted hits its deadline
  // mid-frame, the un-dispatched tail is copied here.  pending_dispatch_pos_
  // tracks how far the resume drain has progressed; both fields are
  // cleared on Condemn.
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
