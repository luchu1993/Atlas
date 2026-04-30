#ifndef ATLAS_LIB_CORO_RPC_CALL_H_
#define ATLAS_LIB_CORO_RPC_CALL_H_

#include <coroutine>
#include <span>
#include <string>

#include "coro/cancellation.h"
#include "coro/pending_rpc_registry.h"
#include "foundation/error.h"
#include "network/channel.h"
#include "network/message.h"
#include "serialization/binary_stream.h"

namespace atlas {

template <typename T>
concept RpcReplyMessage = NetworkMessage<T> && requires(const T& msg) {
  { msg.request_id } -> std::convertible_to<uint32_t>;
};

template <RpcReplyMessage Reply, NetworkMessage Request>
auto RpcCall(PendingRpcRegistry& registry, Channel& channel, const Request& request,
             Duration timeout, CancellationToken token = {}) {
  struct Awaiter {
    PendingRpcRegistry& registry;
    Channel& channel;
    Request request;
    Duration timeout{};
    CancellationToken token;

    Result<Reply> result{Error{ErrorCode::kInternalError, "rpc_call: not completed"}};
    PendingRpcRegistry::PendingHandle pending_handle{};
    CancelRegistration cancel_reg{};

    auto await_ready() -> bool {
      if (token.IsCancelled()) {
        result = Error{ErrorCode::kCancelled, "rpc_call: already cancelled"};
        return true;
      }
      return false;
    }

    auto await_suspend(std::coroutine_handle<> caller) -> std::coroutine_handle<> {
      auto send_result = channel.SendMessage(request);
      if (!send_result) {
        result = Error{send_result.Error().Code(), std::string("rpc_call: send failed: ") +
                                                       std::string(send_result.Error().Message())};
        return caller;  // symmetric transfer back to caller
      }

      auto reply_id = Reply::Descriptor().id;
      auto request_id = request.request_id;

      pending_handle = registry.RegisterPending(
          reply_id, request_id,
          [this, caller](std::span<const std::byte> payload) mutable {
            BinaryReader reader(payload);
            auto reply = Reply::Deserialize(reader);
            if (reply.HasValue())
              result = std::move(reply.Value());
            else
              result = reply.Error();
            caller.resume();
          },
          [this, caller](Error error) mutable {
            result = std::move(error);
            caller.resume();
          },
          timeout);

      if (token.IsValid()) {
        cancel_reg = token.OnCancel([this]() { registry.Cancel(pending_handle); });
      }

      return std::noop_coroutine();  // suspend until reply/timeout/cancel
    }

    auto await_resume() -> Result<Reply> { return std::move(result); }
  };

  return Awaiter{registry, channel, request, timeout, std::move(token)};
}

}  // namespace atlas

#endif  // ATLAS_LIB_CORO_RPC_CALL_H_
