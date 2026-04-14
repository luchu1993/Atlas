#include "network/channel.hpp"

#include "foundation/log.hpp"
#include "network/event_dispatcher.hpp"
#include "network/interface_table.hpp"

#include <cassert>

namespace atlas
{

Channel::Channel(EventDispatcher& dispatcher, InterfaceTable& table, const Address& remote)
    : dispatcher_(dispatcher), interface_table_(table), remote_(remote)
{
}

Channel::~Channel()
{
    if (inactivity_timer_.is_valid())
    {
        dispatcher_.cancel_timer(inactivity_timer_);
    }
}

void Channel::activate()
{
    if (state_ == ChannelState::Created)
    {
        state_ = ChannelState::Active;
        last_activity_ = Clock::now();
        start_inactivity_timer();
    }
}

void Channel::condemn()
{
    if (state_ == ChannelState::Condemned)
    {
        return;
    }
    state_ = ChannelState::Condemned;
    if (inactivity_timer_.is_valid())
    {
        dispatcher_.cancel_timer(inactivity_timer_);
        inactivity_timer_ = TimerHandle{};
    }
    bundle_.clear();
    on_condemned();
}

auto Channel::send() -> Result<void>
{
    if (state_ == ChannelState::Condemned)
    {
        return Error(ErrorCode::ChannelCondemned, "Cannot send on condemned channel");
    }
    if (bundle_.empty())
    {
        return Result<void>{};
    }

    auto data = bundle_.finalize();

    if (packet_filter_)
    {
        auto filtered =
            packet_filter_->send_filter(std::span<const std::byte>(data.data(), data.size()));
        if (!filtered)
        {
            return filtered.error();
        }
        data = std::move(*filtered);
    }

    auto result = do_send(data);
    if (!result)
    {
        return result.error();
    }
    bytes_sent_ += *result;
    return Result<void>{};
}

auto Channel::send_unreliable() -> Result<void>
{
    // Default: TCP (and other reliable-only channels) have no unreliable path —
    // fall back to the normal reliable send so callers need no channel-type check.
    return send();
}

auto Channel::send_message(MessageID id, std::span<const std::byte> data) -> Result<void>
{
    MessageDesc desc{id, "", MessageLengthStyle::Variable, -1};
    if (auto* found = interface_table_.find(id))
    {
        desc = *found;
    }
    bundle_.start_message(desc);
    bundle_.writer().write_bytes(data);
    bundle_.end_message();
    if (desc.is_unreliable())
        return send_unreliable();
    return send();
}

void Channel::set_inactivity_timeout(Duration timeout)
{
    inactivity_timeout_ = timeout;
    last_activity_ = Clock::now();
    start_inactivity_timer();
}

void Channel::reset_inactivity_timer()
{
    last_activity_ = Clock::now();
}

void Channel::start_inactivity_timer()
{
    if (inactivity_timeout_ <= Duration::zero())
    {
        return;
    }
    if (inactivity_timer_.is_valid())
    {
        dispatcher_.cancel_timer(inactivity_timer_);
    }
    inactivity_timer_ = dispatcher_.add_repeating_timer(
        inactivity_timeout_, [this](TimerHandle) { check_inactivity(); });
}

void Channel::check_inactivity()
{
    if (Clock::now() - last_activity_ >= inactivity_timeout_)
    {
        ATLAS_LOG_WARNING("Channel to {} timed out due to inactivity", remote_.to_string());
        on_disconnect();
    }
}

void Channel::on_data_received(std::span<const std::byte> data)
{
    bytes_received_ += data.size();
    last_activity_ = Clock::now();
}

void Channel::on_disconnect()
{
    condemn();
    if (disconnect_callback_)
    {
        disconnect_callback_(*this);
    }
}

void Channel::dispatch_messages(std::span<const std::byte> frame_data)
{
    BinaryReader reader(frame_data);
    while (reader.remaining() >= 1)
    {
        // Read packed MessageID: 1 byte if < 0xFE, else 0xFE + uint16 LE
        auto tag_result = reader.read<uint8_t>();
        if (!tag_result)
        {
            break;
        }
        MessageID id;
        if (*tag_result < 0xFE)
        {
            id = static_cast<MessageID>(*tag_result);
        }
        else if (*tag_result == 0xFE)
        {
            auto id16 = reader.read<uint16_t>();
            if (!id16)
            {
                break;
            }
            id = *id16;
        }
        else
        {
            ATLAS_LOG_WARNING("Invalid packed MessageID tag 0xFF from {}", remote_.to_string());
            break;
        }

        const auto* entry = interface_table_.find_entry(id);
        std::size_t payload_size = 0;

        if (entry && entry->desc.is_fixed())
        {
            payload_size = static_cast<std::size_t>(entry->desc.fixed_length);
        }
        else
        {
            auto len = reader.read_packed_int();
            if (!len)
            {
                break;
            }
            payload_size = *len;
        }

        if (reader.remaining() < payload_size)
        {
            ATLAS_LOG_WARNING("Truncated message {} from {}", id, remote_.to_string());
            break;
        }

        auto payload_span = reader.read_bytes(payload_size);
        if (!payload_span)
        {
            break;
        }

        // Let pre-dispatch hook (RPC registry) consume reply messages first
        if (interface_table_.try_pre_dispatch(id, *payload_span))
        {
            continue;
        }

        if (!entry)
        {
            ATLAS_LOG_WARNING("Unknown message ID {} from {}", id, remote_.to_string());
            continue;
        }

        BinaryReader msg_reader(*payload_span);
        entry->handler->handle_message(remote_, this, id, msg_reader);
    }
}

void Channel::set_disconnect_callback(DisconnectCallback cb)
{
    disconnect_callback_ = std::move(cb);
}

void Channel::set_channel_id(ChannelId id)
{
    channel_id_ = id;
}

}  // namespace atlas
