#pragma once

#include "coro/cancellation.hpp"
#include "coro/pending_rpc_registry.hpp"
#include "foundation/error.hpp"
#include "network/channel.hpp"
#include "network/message.hpp"
#include "serialization/binary_stream.hpp"

#include <coroutine>
#include <span>
#include <string>

namespace atlas
{

// Concept: Reply message must have a request_id field
template <typename T>
concept RpcReplyMessage = NetworkMessage<T> && requires(const T& msg) {
    { msg.request_id } -> std::convertible_to<uint32_t>;
};

// co_await rpc_call<ReplyMsg>(registry, channel, request, timeout);
// co_await rpc_call<ReplyMsg>(registry, channel, request, timeout, cancel_token);
// Returns Result<ReplyMsg>.
template <RpcReplyMessage Reply, NetworkMessage Request>
auto rpc_call(PendingRpcRegistry& registry, Channel& channel, const Request& request,
              Duration timeout, CancellationToken token = {})
{
    struct Awaiter
    {
        PendingRpcRegistry& registry;
        Channel& channel;
        Request request;
        Duration timeout;
        CancellationToken token;

        Result<Reply> result{Error{ErrorCode::InternalError, "rpc_call: not completed"}};
        PendingRpcRegistry::PendingHandle pending_handle{};
        CancelRegistration cancel_reg{};

        auto await_ready() -> bool
        {
            if (token.is_cancelled())
            {
                result = Error{ErrorCode::Cancelled, "rpc_call: already cancelled"};
                return true;
            }
            return false;
        }

        auto await_suspend(std::coroutine_handle<> caller) -> std::coroutine_handle<>
        {
            // 1. Send request
            auto send_result = channel.send_message(request);
            if (!send_result)
            {
                result = Error{send_result.error().code(),
                               std::string("rpc_call: send failed: ") +
                                   std::string(send_result.error().message())};
                return caller;  // symmetric transfer back to caller
            }

            // 2. Register pending entry
            auto reply_id = Reply::descriptor().id;
            auto request_id = request.request_id;

            pending_handle = registry.register_pending(
                reply_id, request_id,
                // on_reply: deserialize and resume
                [this, caller](std::span<const std::byte> payload) mutable
                {
                    BinaryReader reader(payload);
                    auto reply = Reply::deserialize(reader);
                    if (reply.has_value())
                        result = std::move(reply.value());
                    else
                        result = reply.error();
                    caller.resume();
                },
                // on_error: timeout or cancel
                [this, caller](Error error) mutable
                {
                    result = std::move(error);
                    caller.resume();
                },
                timeout);

            // 3. Register cancellation callback
            if (token.is_valid())
            {
                cancel_reg = token.on_cancel([this]() { registry.cancel(pending_handle); });
            }

            return std::noop_coroutine();  // suspend until reply/timeout/cancel
        }

        auto await_resume() -> Result<Reply> { return std::move(result); }
    };

    return Awaiter{registry, channel, request, timeout, std::move(token)};
}

}  // namespace atlas
