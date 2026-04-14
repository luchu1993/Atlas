#pragma once

#include "foundation/containers/paged_sparse_table.hpp"
#include "network/message.hpp"

#include <functional>
#include <memory>

namespace atlas
{

class InterfaceTable
{
public:
    struct Entry
    {
        MessageDesc desc;
        std::unique_ptr<MessageHandler> handler;
    };

    InterfaceTable() = default;

    [[nodiscard]] auto register_handler(MessageID id, const MessageDesc& desc,
                                        std::unique_ptr<MessageHandler> handler) -> Result<void>;

    template <NetworkMessage Msg>
    auto register_typed_handler(typename TypedMessageHandler<Msg>::Callback callback)
        -> Result<void>
    {
        return register_handler(Msg::descriptor().id, Msg::descriptor(),
                                make_handler<Msg>(std::move(callback)));
    }

    [[nodiscard]] auto dispatch(const Address& source, Channel* channel, MessageID id,
                                BinaryReader& data) -> Result<void>;

    [[nodiscard]] auto find(MessageID id) const -> const MessageDesc*;
    [[nodiscard]] auto find_entry(MessageID id) const -> const Entry*;
    [[nodiscard]] auto handler(MessageID id) const -> MessageHandler*;
    [[nodiscard]] auto handler_count() const -> size_t;

    using DefaultHandler = std::function<void(const Address&, Channel*, MessageID, BinaryReader&)>;
    void set_default_handler(DefaultHandler handler) { default_handler_ = std::move(handler); }

private:
    PagedSparseTable<MessageID, Entry> entries_;
    DefaultHandler default_handler_;
};

}  // namespace atlas
