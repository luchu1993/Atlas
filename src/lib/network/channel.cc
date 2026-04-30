#include "network/channel.h"

#include <cassert>
#include <cstdio>

#include "foundation/log.h"
#include "foundation/profiler.h"
#include "network/event_dispatcher.h"
#include "network/interface_table.h"

namespace atlas {

Channel::Channel(EventDispatcher& dispatcher, InterfaceTable& table, const Address& remote)
    : dispatcher_(dispatcher), interface_table_(table), remote_(remote) {}

Channel::~Channel() {
  if (inactivity_timer_.IsValid()) {
    dispatcher_.CancelTimer(inactivity_timer_);
  }
}

void Channel::Activate() {
  if (state_ == ChannelState::kCreated) {
    state_ = ChannelState::kActive;
    last_activity_ = Clock::now();
    StartInactivityTimer();
  }
}

void Channel::Condemn() {
  if (state_ == ChannelState::kCondemned) {
    return;
  }
  state_ = ChannelState::kCondemned;
  if (inactivity_timer_.IsValid()) {
    dispatcher_.CancelTimer(inactivity_timer_);
    inactivity_timer_ = TimerHandle{};
  }
  bundle_.Clear();
  OnCondemned();
}

auto Channel::Send() -> Result<void> {
  ATLAS_PROFILE_ZONE_N("Channel::Send");
  if (state_ == ChannelState::kCondemned) {
    bundle_.Clear();
    return Error(ErrorCode::kChannelCondemned, "Cannot send on condemned channel");
  }
  if (bundle_.empty()) {
    return Result<void>{};
  }

  auto data = bundle_.Finalize();

  if (packet_filter_) {
    auto filtered =
        packet_filter_->SendFilter(std::span<const std::byte>(data.data(), data.size()));
    if (!filtered) {
      ATLAS_LOG_WARNING("Channel send_filter failed for {}: {} ({} bytes lost)", remote_.ToString(),
                        filtered.Error().Message(), data.size());
      return filtered.Error();
    }
    data = std::move(*filtered);
  }

  auto result = DoSend(data);
  if (!result) {
    return result.Error();
  }
  bytes_sent_ += *result;
  ATLAS_PROFILE_PLOT("BytesOut", static_cast<double>(*result));
  return Result<void>{};
}

auto Channel::SendUnreliable() -> Result<void> {
  return Send();
}

auto Channel::CondemnedSendError() const -> Result<void> {
  ATLAS_LOG_DEBUG("SendMessage on condemned channel to {}", remote_.ToString());
  return Error(ErrorCode::kChannelCondemned, "Cannot send on condemned channel");
}

void Channel::SetInactivityTimeout(Duration timeout) {
  inactivity_timeout_ = timeout;
  last_activity_ = Clock::now();
  StartInactivityTimer();
}

void Channel::ResetInactivityTimer() {
  last_activity_ = Clock::now();
}

void Channel::StartInactivityTimer() {
  if (inactivity_timeout_ <= Duration::zero()) {
    return;
  }
  if (inactivity_timer_.IsValid()) {
    dispatcher_.CancelTimer(inactivity_timer_);
  }
  inactivity_timer_ = dispatcher_.AddRepeatingTimer(inactivity_timeout_,
                                                    [this](TimerHandle) { CheckInactivity(); });
}

void Channel::CheckInactivity() {
  if (Clock::now() - last_activity_ >= inactivity_timeout_) {
    ATLAS_LOG_WARNING("Channel to {} timed out due to inactivity", remote_.ToString());
    OnDisconnect();
  }
}

void Channel::OnDataReceived(std::span<const std::byte> data) {
  bytes_received_ += data.size();
  last_activity_ = Clock::now();
  ATLAS_PROFILE_PLOT("BytesIn", static_cast<double>(data.size()));
}

void Channel::OnDisconnect() {
  if (state_ == ChannelState::kCondemned) {
    return;
  }
  Condemn();
  if (disconnect_callback_) {
    disconnect_callback_(*this);
  }
}

namespace {

// Returns the next safe byte offset; parse errors consume the frame so resume
// never retries corrupt bytes.
auto DispatchFrameImpl(InterfaceTable& interface_table, const Address& remote, Channel* channel,
                       std::span<const std::byte> frame_data, TimePoint deadline) -> std::size_t {
  ATLAS_PROFILE_ZONE_N("Channel::Dispatch");
  BinaryReader reader(frame_data);
  std::size_t last_safe_pos = 0;
  while (reader.Remaining() >= 1) {
    auto tag_result = reader.Read<uint8_t>();
    if (!tag_result) {
      return frame_data.size();
    }
    MessageID id;
    if (*tag_result < 0xFE) {
      id = static_cast<MessageID>(*tag_result);
    } else if (*tag_result == 0xFE) {
      auto id16 = reader.Read<uint16_t>();
      if (!id16) {
        return frame_data.size();
      }
      id = *id16;
    } else {
      ATLAS_LOG_WARNING("Invalid packed MessageID tag 0xFF from {}", remote.ToString());
      return frame_data.size();
    }

    const auto* entry = interface_table.FindEntry(id);
    std::size_t payload_size = 0;

    if (entry && entry->desc.IsFixed()) {
      payload_size = static_cast<std::size_t>(entry->desc.fixed_length);
    } else {
      auto len = reader.ReadPackedInt();
      if (!len) {
        return frame_data.size();
      }
      payload_size = *len;
    }

    if (reader.Remaining() < payload_size) {
      ATLAS_LOG_WARNING("Truncated message {} from {}", id, remote.ToString());
      return frame_data.size();
    }

    auto payload_span = reader.ReadBytes(payload_size);
    if (!payload_span) {
      return frame_data.size();
    }

    if (interface_table.TryPreDispatch(id, *payload_span)) {
      last_safe_pos = reader.Position();
      if (Clock::now() >= deadline) return last_safe_pos;
      continue;
    }

    {
      ATLAS_PROFILE_ZONE_N("Channel::HandleMessage");
#if ATLAS_PROFILE_ENABLED
      {
        char id_buf[16];
        int id_len = std::snprintf(id_buf, sizeof(id_buf), "id=%u", static_cast<unsigned>(id));
        if (id_len > 0) {
          ATLAS_PROFILE_ZONE_TEXT(id_buf, static_cast<size_t>(id_len));
        }
      }
#endif
      BinaryReader msg_reader(*payload_span);
      if (!entry) {
        if (!interface_table.TryDispatchDefault(remote, channel, id, msg_reader)) {
          ATLAS_LOG_WARNING("Unknown message ID {} from {}", id, remote.ToString());
        }
      } else {
        entry->handler->HandleMessage(remote, channel, id, msg_reader);
      }
    }

    last_safe_pos = reader.Position();
    if (Clock::now() >= deadline) return last_safe_pos;
  }
  return frame_data.size();
}

}  // namespace

void Channel::DispatchMessages(std::span<const std::byte> frame_data) {
  (void)DispatchFrameImpl(interface_table_, remote_, this, frame_data, TimePoint::max());
}

auto Channel::DispatchMessagesBudgeted(std::span<const std::byte> frame_data, TimePoint deadline)
    -> bool {
  if (frame_data.empty()) {
    return HasPendingDispatch();
  }
  const auto consumed = DispatchFrameImpl(interface_table_, remote_, this, frame_data, deadline);
  if (consumed < frame_data.size()) {
    if (pending_dispatch_pos_ > 0) {
      pending_dispatch_buf_.erase(
          pending_dispatch_buf_.begin(),
          pending_dispatch_buf_.begin() + static_cast<std::ptrdiff_t>(pending_dispatch_pos_));
      pending_dispatch_pos_ = 0;
    }
    auto tail = frame_data.subspan(consumed);
    pending_dispatch_buf_.insert(pending_dispatch_buf_.end(), tail.begin(), tail.end());
  }
  return HasPendingDispatch();
}

auto Channel::DrainPendingDispatch(TimePoint deadline) -> bool {
  if (!HasPendingDispatch()) return false;
  std::span<const std::byte> tail{pending_dispatch_buf_.data() + pending_dispatch_pos_,
                                  pending_dispatch_buf_.size() - pending_dispatch_pos_};
  const auto consumed = DispatchFrameImpl(interface_table_, remote_, this, tail, deadline);
  pending_dispatch_pos_ += consumed;
  if (!HasPendingDispatch()) {
    pending_dispatch_buf_.clear();
    pending_dispatch_buf_.shrink_to_fit();
    pending_dispatch_pos_ = 0;
    return false;
  }
  return true;
}

void Channel::SetDisconnectCallback(DisconnectCallback cb) {
  disconnect_callback_ = std::move(cb);
}

void Channel::SetChannelId(::atlas::ChannelId id) {
  channel_id_ = id;
}

}  // namespace atlas
