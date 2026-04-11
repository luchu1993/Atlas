#include "network/interface_table.hpp"

#include "foundation/log.hpp"

namespace atlas
{

auto InterfaceTable::register_handler(MessageID id, const MessageDesc& desc,
                                      std::unique_ptr<MessageHandler> handler) -> Result<void>
{
    if (entries_[id])
    {
        return Error(ErrorCode::AlreadyExists, "Message ID already registered");
    }

    entries_[id] = std::make_unique<Entry>(Entry{desc, std::move(handler)});
    ++count_;
    return {};
}

auto InterfaceTable::dispatch(const Address& source, Channel* channel, MessageID id,
                              BinaryReader& data) -> Result<void>
{
    auto* entry = entries_[id].get();
    if (!entry)
    {
        return Error(ErrorCode::NotFound, "Unknown message ID");
    }

    entry->handler->handle_message(source, channel, id, data);
    return {};
}

auto InterfaceTable::find(MessageID id) const -> const MessageDesc*
{
    auto* entry = entries_[id].get();
    if (!entry)
    {
        return nullptr;
    }
    return &entry->desc;
}

auto InterfaceTable::find_entry(MessageID id) const -> const Entry*
{
    return entries_[id].get();
}

auto InterfaceTable::handler(MessageID id) const -> MessageHandler*
{
    auto* entry = entries_[id].get();
    if (!entry)
    {
        return nullptr;
    }
    return entry->handler.get();
}

auto InterfaceTable::handler_count() const -> size_t
{
    return count_;
}

}  // namespace atlas
