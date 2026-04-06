#include "network/interface_table.hpp"
#include "foundation/log.hpp"

#include <format>
#include <stdexcept>

namespace atlas
{

auto InterfaceTable::register_handler(MessageID id, const MessageDesc& desc,
                                       std::shared_ptr<MessageHandler> handler)
    -> Result<void>
{
    if (entries_.contains(id))
    {
        return Error(ErrorCode::AlreadyExists,
            std::format("Message ID {} already registered", id));
    }

    entries_.emplace(id, Entry{desc, std::move(handler)});
    return {};
}

auto InterfaceTable::dispatch(const Address& source, Channel* channel,
                               MessageID id, BinaryReader& data) -> Result<void>
{
    auto it = entries_.find(id);
    if (it == entries_.end())
    {
        return Error(ErrorCode::NotFound,
            std::format("Unknown message ID: {}", id));
    }

    try
    {
        it->second.handler->handle_message(source, channel, id, data);
    }
    catch (const std::exception& e)
    {
        ATLAS_LOG_ERROR("Handler exception for message {}: {}", id, e.what());
        return Error(ErrorCode::InternalError, e.what());
    }
    catch (...)
    {
        ATLAS_LOG_ERROR("Unknown handler exception for message {}", id);
        return Error(ErrorCode::InternalError, "Unknown exception in message handler");
    }

    return {};
}

auto InterfaceTable::find(MessageID id) const -> const MessageDesc*
{
    auto it = entries_.find(id);
    if (it == entries_.end())
    {
        return nullptr;
    }
    return &it->second.desc;
}

auto InterfaceTable::handler(MessageID id) const -> MessageHandler*
{
    auto it = entries_.find(id);
    if (it == entries_.end())
    {
        return nullptr;
    }
    return it->second.handler.get();
}

auto InterfaceTable::handler_count() const -> size_t
{
    return entries_.size();
}

} // namespace atlas
