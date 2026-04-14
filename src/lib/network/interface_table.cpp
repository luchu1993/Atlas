#include "network/interface_table.hpp"

#include "foundation/log.hpp"
#include "serialization/binary_stream.hpp"

namespace atlas
{

auto InterfaceTable::register_handler(MessageID id, const MessageDesc& desc,
                                      std::unique_ptr<MessageHandler> handler) -> Result<void>
{
    if (entries_.contains(id))
    {
        return Error(ErrorCode::AlreadyExists, "Message ID already registered");
    }

    (void)entries_.insert(id, std::make_unique<Entry>(Entry{desc, std::move(handler)}));
    return {};
}

auto InterfaceTable::dispatch(const Address& source, Channel* channel, MessageID id,
                              BinaryReader& data) -> Result<void>
{
    // Pre-dispatch hook: let coroutine RPC registry consume reply messages
    if (pre_dispatch_hook_)
    {
        auto payload = data.data().subspan(data.position());
        if (pre_dispatch_hook_(id, payload))
        {
            data.skip(payload.size());  // advance reader past consumed message
            return {};
        }
    }

    auto* entry = entries_.get(id);
    if (!entry)
    {
        if (default_handler_)
        {
            default_handler_(source, channel, id, data);
            return {};
        }
        return Error(ErrorCode::NotFound, "Unknown message ID");
    }

    entry->handler->handle_message(source, channel, id, data);
    return {};
}

auto InterfaceTable::find(MessageID id) const -> const MessageDesc*
{
    auto* entry = entries_.get(id);
    if (!entry)
    {
        return nullptr;
    }
    return &entry->desc;
}

auto InterfaceTable::find_entry(MessageID id) const -> const Entry*
{
    return entries_.get(id);
}

auto InterfaceTable::handler(MessageID id) const -> MessageHandler*
{
    auto* entry = entries_.get(id);
    if (!entry)
    {
        return nullptr;
    }
    return entry->handler.get();
}

auto InterfaceTable::handler_count() const -> size_t
{
    return entries_.size();
}

}  // namespace atlas
