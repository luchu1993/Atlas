#pragma once

#include "network/message.hpp"

#include <unordered_map>

namespace atlas
{

class InterfaceTable
{
public:
    InterfaceTable() = default;

    [[nodiscard]] auto register_handler(MessageID id, const MessageDesc& desc,
                                         std::shared_ptr<MessageHandler> handler)
        -> Result<void>;

    // Convenience: register typed handler
    template <NetworkMessage Msg>
    auto register_typed_handler(typename TypedMessageHandler<Msg>::Callback callback)
        -> Result<void>
    {
        return register_handler(
            Msg::descriptor().id,
            Msg::descriptor(),
            make_handler<Msg>(std::move(callback)));
    }

    [[nodiscard]] auto dispatch(const Address& source, Channel* channel,
                                 MessageID id, BinaryReader& data) -> Result<void>;

    [[nodiscard]] auto find(MessageID id) const -> const MessageDesc*;
    [[nodiscard]] auto handler(MessageID id) const -> MessageHandler*;
    [[nodiscard]] auto handler_count() const -> size_t;

private:
    struct Entry
    {
        MessageDesc desc;
        std::shared_ptr<MessageHandler> handler;
    };

    std::unordered_map<MessageID, Entry> entries_;
};

} // namespace atlas
