#include "network/channel.h"

#include <cassert>

#include "foundation/log.h"
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
  bundle_.clear();
  OnCondemned();
}

auto Channel::Send() -> Result<void> {
  if (state_ == ChannelState::kCondemned) {
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
      return filtered.Error();
    }
    data = std::move(*filtered);
  }

  auto result = DoSend(data);
  if (!result) {
    return result.Error();
  }
  bytes_sent_ += *result;
  return Result<void>{};
}

auto Channel::SendUnreliable() -> Result<void> {
  // Default: TCP (and other reliable-only channels) have no unreliable path —
  // fall back to the normal reliable send so callers need no channel-type check.
  return Send();
}

auto Channel::SendMessage(MessageID id, std::span<const std::byte> data) -> Result<void> {
  MessageDesc desc{id, "", MessageLengthStyle::kVariable, -1};
  if (auto* found = interface_table_.Find(id)) {
    desc = *found;
  }
  bundle_.StartMessage(desc);
  bundle_.Writer().WriteBytes(data);
  bundle_.EndMessage();
  if (desc.IsUnreliable()) return SendUnreliable();
  return Send();
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
}

void Channel::OnDisconnect() {
  Condemn();
  if (disconnect_callback_) {
    disconnect_callback_(*this);
  }
}

void Channel::DispatchMessages(std::span<const std::byte> frame_data) {
  BinaryReader reader(frame_data);
  while (reader.Remaining() >= 1) {
    // Read packed MessageID: 1 byte if < 0xFE, else 0xFE + uint16 LE
    auto tag_result = reader.Read<uint8_t>();
    if (!tag_result) {
      break;
    }
    MessageID id;
    if (*tag_result < 0xFE) {
      id = static_cast<MessageID>(*tag_result);
    } else if (*tag_result == 0xFE) {
      auto id16 = reader.Read<uint16_t>();
      if (!id16) {
        break;
      }
      id = *id16;
    } else {
      ATLAS_LOG_WARNING("Invalid packed MessageID tag 0xFF from {}", remote_.ToString());
      break;
    }

    const auto* entry = interface_table_.FindEntry(id);
    std::size_t payload_size = 0;

    if (entry && entry->desc.IsFixed()) {
      payload_size = static_cast<std::size_t>(entry->desc.fixed_length);
    } else {
      auto len = reader.ReadPackedInt();
      if (!len) {
        break;
      }
      payload_size = *len;
    }

    if (reader.Remaining() < payload_size) {
      ATLAS_LOG_WARNING("Truncated message {} from {}", id, remote_.ToString());
      break;
    }

    auto payload_span = reader.ReadBytes(payload_size);
    if (!payload_span) {
      break;
    }

    // Let pre-dispatch hook (RPC registry) consume reply messages first
    if (interface_table_.TryPreDispatch(id, *payload_span)) {
      continue;
    }

    if (!entry) {
      ATLAS_LOG_WARNING("Unknown message ID {} from {}", id, remote_.ToString());
      continue;
    }

    BinaryReader msg_reader(*payload_span);
    entry->handler->HandleMessage(remote_, this, id, msg_reader);
  }
}

void Channel::SetDisconnectCallback(DisconnectCallback cb) {
  disconnect_callback_ = std::move(cb);
}

void Channel::SetChannelId(::atlas::ChannelId id) {
  channel_id_ = id;
}

}  // namespace atlas
