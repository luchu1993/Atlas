#ifndef ATLAS_LIB_NETWORK_INTERFACE_TABLE_H_
#define ATLAS_LIB_NETWORK_INTERFACE_TABLE_H_

#include <cstddef>
#include <functional>
#include <memory>
#include <span>

#include "foundation/containers/paged_sparse_table.h"
#include "network/message.h"

namespace atlas {

class InterfaceTable {
 public:
  struct Entry {
    MessageDesc desc;
    std::unique_ptr<MessageHandler> handler;
  };

  InterfaceTable() = default;

  [[nodiscard]] auto RegisterHandler(MessageID id, const MessageDesc& desc,
                                     std::unique_ptr<MessageHandler> handler) -> Result<void>;

  template <NetworkMessage Msg>
  auto RegisterTypedHandler(typename TypedMessageHandler<Msg>::Callback callback) -> Result<void> {
    return RegisterHandler(Msg::Descriptor().id, Msg::Descriptor(),
                           MakeHandler<Msg>(std::move(callback)));
  }

  [[nodiscard]] auto Dispatch(const Address& source, Channel* channel, MessageID id,
                              BinaryReader& data) -> Result<void>;

  [[nodiscard]] auto Find(MessageID id) const -> const MessageDesc*;
  [[nodiscard]] auto FindEntry(MessageID id) const -> const Entry*;
  [[nodiscard]] auto Handler(MessageID id) const -> MessageHandler*;
  [[nodiscard]] auto HandlerCount() const -> size_t;

  using DefaultHandler = std::function<void(const Address&, Channel*, MessageID, BinaryReader&)>;
  void SetDefaultHandler(DefaultHandler handler) { default_handler_ = std::move(handler); }

  // Pre-dispatch hook: if set, called before normal dispatch.
  // Returns true if the message was consumed (e.g. by a coroutine RPC registry).
  using PreDispatchHook = std::function<bool(MessageID, std::span<const std::byte>)>;
  void SetPreDispatchHook(PreDispatchHook hook) { pre_dispatch_hook_ = std::move(hook); }

  // Try the pre-dispatch hook directly (used by Channel for unregistered messages).
  auto TryPreDispatch(MessageID id, std::span<const std::byte> payload) -> bool {
    if (pre_dispatch_hook_) return pre_dispatch_hook_(id, payload);
    return false;
  }

  // Invoke the registered default handler for an unrecognized MessageID.
  // Returns false when no default handler has been installed, in which
  // case the caller should log and drop. Used by Channel's inline per-
  // message loop (which does its own typed-entry lookup and skips
  // Dispatch()) so the client's state-channel fallback still runs for
  // 0xF001 / 0xF002 / 0xF003.
  auto TryDispatchDefault(const Address& source, Channel* channel, MessageID id, BinaryReader& data)
      -> bool {
    if (!default_handler_) return false;
    default_handler_(source, channel, id, data);
    return true;
  }

 private:
  PagedSparseTable<MessageID, Entry> entries_;
  DefaultHandler default_handler_;
  PreDispatchHook pre_dispatch_hook_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_NETWORK_INTERFACE_TABLE_H_
