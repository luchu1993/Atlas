#include "network/channel.hpp"
#include "network/event_dispatcher.hpp"
#include "network/interface_table.hpp"
#include "foundation/log.hpp"

#include <cassert>

namespace atlas
{

Channel::Channel(EventDispatcher& dispatcher, InterfaceTable& table,
                 const Address& remote)
    : dispatcher_(dispatcher)
    , interface_table_(table)
    , remote_(remote)
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
        reset_inactivity_timer();
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
    auto result = do_send(data);
    if (!result)
    {
        return result.error();
    }
    bytes_sent_ += *result;
    return Result<void>{};
}

auto Channel::send_message(MessageID id, std::span<const std::byte> data)
    -> Result<void>
{
    MessageDesc desc{id, "", MessageLengthStyle::Variable, -1};
    if (auto* found = interface_table_.find(id))
    {
        desc = *found;
    }
    bundle_.start_message(desc);
    bundle_.writer().write_bytes(data);
    bundle_.end_message();
    return send();
}

void Channel::set_inactivity_timeout(Duration timeout)
{
    inactivity_timeout_ = timeout;
    reset_inactivity_timer();
}

void Channel::reset_inactivity_timer()
{
    if (inactivity_timeout_ <= Duration::zero())
    {
        return;
    }
    if (inactivity_timer_.is_valid())
    {
        dispatcher_.cancel_timer(inactivity_timer_);
    }
    inactivity_timer_ = dispatcher_.add_timer(inactivity_timeout_,
        [this](TimerHandle)
        {
            ATLAS_LOG_WARNING("Channel to {} timed out due to inactivity",
                remote_.to_string());
            on_disconnect();
        });
}

void Channel::on_data_received(std::span<const std::byte> data)
{
    bytes_received_ += data.size();
    reset_inactivity_timer();
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
    while (reader.remaining() >= sizeof(MessageID))
    {
        // BinaryReader::read<T> already handles endian conversion
        auto id_result = reader.read<MessageID>();
        if (!id_result)
        {
            break;
        }
        auto id = *id_result;

        // Determine payload size
        const auto* desc = interface_table_.find(id);
        std::size_t payload_size = 0;

        if (desc && desc->is_fixed())
        {
            payload_size = static_cast<std::size_t>(desc->fixed_length);
        }
        else
        {
            // Variable: read packed int length
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

        // Extract this message's payload bytes
        auto payload_span = reader.read_bytes(payload_size);
        if (!payload_span)
        {
            break;
        }

        BinaryReader msg_reader(*payload_span);
        auto dispatch_result = interface_table_.dispatch(remote_, this, id, msg_reader);
        if (!dispatch_result)
        {
            ATLAS_LOG_WARNING("Failed to dispatch message {}: {}",
                id, dispatch_result.error().message());
        }
    }
}

void Channel::set_disconnect_callback(DisconnectCallback cb)
{
    disconnect_callback_ = std::move(cb);
}

} // namespace atlas
