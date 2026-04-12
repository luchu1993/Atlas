#include "machined/watcher_forwarder.hpp"
#include "network/event_dispatcher.hpp"
#include "network/interface_table.hpp"
#include "platform/io_poller.hpp"
#include "serialization/binary_stream.hpp"

#include <gtest/gtest.h>

#include <unordered_map>

using namespace atlas;
using namespace atlas::machined;

namespace
{

class RecordingChannel final : public Channel
{
public:
    RecordingChannel(EventDispatcher& dispatcher, InterfaceTable& table, const Address& remote)
        : Channel(dispatcher, table, remote)
    {
    }

    [[nodiscard]] auto fd() const -> FdHandle override { return kInvalidFd; }

    [[nodiscard]] auto do_send(std::span<const std::byte> data) -> Result<size_t> override
    {
        last_sent_.assign(data.begin(), data.end());
        return data.size();
    }

    [[nodiscard]] auto last_sent() const -> const std::vector<std::byte>& { return last_sent_; }

private:
    std::vector<std::byte> last_sent_;
};

template <typename Msg>
auto decode_single_bundle_message(const std::vector<std::byte>& encoded) -> Msg
{
    BinaryReader reader(std::span<const std::byte>(encoded.data(), encoded.size()));
    const auto id = reader.read_packed_int();
    EXPECT_TRUE(id.has_value());
    if (!id)
        return Msg{};
    EXPECT_EQ(*id, Msg::descriptor().id);

    std::size_t payload_size = 0;
    if (Msg::descriptor().is_fixed())
    {
        payload_size = static_cast<std::size_t>(Msg::descriptor().fixed_length);
    }
    else
    {
        const auto len = reader.read_packed_int();
        EXPECT_TRUE(len.has_value());
        if (!len)
            return Msg{};
        payload_size = *len;
    }

    const auto payload = reader.read_bytes(payload_size);
    EXPECT_TRUE(payload.has_value());
    if (!payload)
        return Msg{};

    BinaryReader msg_reader(*payload);
    const auto msg = Msg::deserialize(msg_reader);
    EXPECT_TRUE(msg.has_value());
    return msg.value_or(Msg{});
}

}  // namespace

// ============================================================================
// WatcherForwarder unit tests
//
// These tests exercise only the pure bookkeeping logic that does not require
// actual channel I/O.  In particular we test:
//   - pending_count() bookkeeping
//   - check_timeouts() expiry (by relying on a registry with no live channels
//     so handle_request returns early with a not-found reply path)
// ============================================================================

TEST(WatcherForwarder, InitiallyEmpty)
{
    ProcessRegistry reg;
    WatcherForwarder fwd(reg, {});
    EXPECT_EQ(fwd.pending_count(), 0u);
}

// When the target process is not in the registry, handle_request should send a
// not-found response immediately (no pending entry added).
TEST(WatcherForwarder, HandleRequestTargetNotFound)
{
    ProcessRegistry reg;
    WatcherForwarder fwd(reg, {});

    WatcherRequest req;
    req.target_type = ProcessType::BaseApp;
    req.target_name = "no-such";
    req.watcher_path = "server/tick";
    req.request_id = 1;

    // requester_channel = nullptr → guard hits early
    fwd.handle_request(nullptr, req);
    EXPECT_EQ(fwd.pending_count(), 0u);
}

// handle_reply for an unknown request_id is a no-op (no crash)
TEST(WatcherForwarder, HandleReplyUnknownId)
{
    ProcessRegistry reg;
    WatcherForwarder fwd(reg, {});

    WatcherReply reply;
    reply.request_id = 9999;
    reply.found = true;
    reply.value = "42";

    EXPECT_NO_THROW(fwd.handle_reply(nullptr, reply));
    EXPECT_EQ(fwd.pending_count(), 0u);
}

// check_timeouts on empty pending list is a no-op (no crash)
TEST(WatcherForwarder, CheckTimeoutsEmpty)
{
    ProcessRegistry reg;
    WatcherForwarder fwd(reg, {});
    EXPECT_NO_THROW(fwd.check_timeouts());
    EXPECT_EQ(fwd.pending_count(), 0u);
}

TEST(WatcherForwarder, HandleReplyRestoresOriginalRequesterRequestId)
{
    EventDispatcher dispatcher{"watcher_forwarder_test"};
    InterfaceTable table;

    RecordingChannel requester(dispatcher, table, Address("127.0.0.1", 31001));
    RecordingChannel target(dispatcher, table, Address("127.0.0.1", 41001));
    requester.activate();
    target.activate();

    std::unordered_map<Address, Channel*> channels;
    channels.emplace(requester.remote_address(), &requester);

    ProcessRegistry reg;
    ProcessEntry entry;
    entry.process_type = ProcessType::BaseApp;
    entry.name = "baseapp_01";
    entry.internal_addr = Address("127.0.0.1", 41001);
    entry.channel = &target;
    ASSERT_TRUE(reg.register_process(std::move(entry)));

    WatcherForwarder fwd(reg,
                         [&channels](const Address& addr) -> Channel*
                         {
                             const auto it = channels.find(addr);
                             return (it != channels.end()) ? it->second : nullptr;
                         });

    WatcherRequest req;
    req.target_type = ProcessType::BaseApp;
    req.target_name = "baseapp_01";
    req.watcher_path = "server/tick";
    req.request_id = 42;

    fwd.handle_request(&requester, req);
    ASSERT_EQ(fwd.pending_count(), 1u);

    const WatcherForward forwarded =
        decode_single_bundle_message<WatcherForward>(target.last_sent());
    EXPECT_EQ(forwarded.watcher_path, req.watcher_path);
    EXPECT_NE(forwarded.request_id, req.request_id);

    WatcherReply reply;
    reply.request_id = forwarded.request_id;
    reply.found = true;
    reply.value = "123";

    fwd.handle_reply(&target, reply);
    EXPECT_EQ(fwd.pending_count(), 0u);

    const WatcherResponse resp =
        decode_single_bundle_message<WatcherResponse>(requester.last_sent());
    EXPECT_EQ(resp.request_id, req.request_id);
    EXPECT_TRUE(resp.found);
    EXPECT_EQ(resp.source_name, "baseapp_01");
    EXPECT_EQ(resp.value, "123");
}

TEST(WatcherForwarder, HandleReplyWithRequesterGoneIsSafe)
{
    EventDispatcher dispatcher{"watcher_forwarder_test"};
    InterfaceTable table;

    RecordingChannel requester(dispatcher, table, Address("127.0.0.1", 31002));
    RecordingChannel target(dispatcher, table, Address("127.0.0.1", 41002));
    requester.activate();
    target.activate();

    std::unordered_map<Address, Channel*> channels;
    channels.emplace(requester.remote_address(), &requester);

    ProcessRegistry reg;
    ProcessEntry entry;
    entry.process_type = ProcessType::BaseApp;
    entry.name = "baseapp_02";
    entry.internal_addr = Address("127.0.0.1", 41002);
    entry.channel = &target;
    ASSERT_TRUE(reg.register_process(std::move(entry)));

    WatcherForwarder fwd(reg,
                         [&channels](const Address& addr) -> Channel*
                         {
                             const auto it = channels.find(addr);
                             return (it != channels.end()) ? it->second : nullptr;
                         });

    WatcherRequest req;
    req.target_type = ProcessType::BaseApp;
    req.target_name = "baseapp_02";
    req.watcher_path = "server/tick";
    req.request_id = 7;

    fwd.handle_request(&requester, req);
    ASSERT_EQ(fwd.pending_count(), 1u);

    const WatcherForward forwarded =
        decode_single_bundle_message<WatcherForward>(target.last_sent());
    channels.erase(requester.remote_address());
    requester.condemn();

    WatcherReply reply;
    reply.request_id = forwarded.request_id;
    reply.found = true;
    reply.value = "ignored";

    EXPECT_NO_THROW(fwd.handle_reply(&target, reply));
    EXPECT_EQ(fwd.pending_count(), 0u);
}
