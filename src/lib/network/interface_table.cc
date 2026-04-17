#include "network/interface_table.h"

#include "foundation/log.h"
#include "serialization/binary_stream.h"

namespace atlas {

auto InterfaceTable::RegisterHandler(MessageID id, const MessageDesc& desc,
                                     std::unique_ptr<MessageHandler> handler) -> Result<void> {
  if (entries_.Contains(id)) {
    return Error(ErrorCode::kAlreadyExists, "Message ID already registered");
  }

  (void)entries_.Insert(id, std::make_unique<Entry>(Entry{desc, std::move(handler)}));
  return {};
}

auto InterfaceTable::Dispatch(const Address& source, Channel* channel, MessageID id,
                              BinaryReader& data) -> Result<void> {
  // Pre-dispatch hook: let coroutine RPC registry consume reply messages
  if (pre_dispatch_hook_) {
    auto payload = data.data().subspan(data.Position());
    if (pre_dispatch_hook_(id, payload)) {
      data.Skip(payload.size());  // advance reader past consumed message
      return {};
    }
  }

  auto* entry = entries_.Get(id);
  if (!entry) {
    if (default_handler_) {
      default_handler_(source, channel, id, data);
      return {};
    }
    return Error(ErrorCode::kNotFound, "Unknown message ID");
  }

  entry->handler->HandleMessage(source, channel, id, data);
  return {};
}

auto InterfaceTable::Find(MessageID id) const -> const MessageDesc* {
  auto* entry = entries_.Get(id);
  if (!entry) {
    return nullptr;
  }
  return &entry->desc;
}

auto InterfaceTable::FindEntry(MessageID id) const -> const Entry* {
  return entries_.Get(id);
}

auto InterfaceTable::Handler(MessageID id) const -> MessageHandler* {
  auto* entry = entries_.Get(id);
  if (!entry) {
    return nullptr;
  }
  return entry->handler.get();
}

auto InterfaceTable::HandlerCount() const -> size_t {
  return entries_.size();
}

}  // namespace atlas
