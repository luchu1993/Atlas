#include "machined/watcher_forwarder.hpp"

#include <gtest/gtest.h>

using namespace atlas;
using namespace atlas::machined;

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
    WatcherForwarder fwd(reg);
    EXPECT_EQ(fwd.pending_count(), 0u);
}

// When the target process is not in the registry, handle_request should send a
// not-found response immediately (no pending entry added).
TEST(WatcherForwarder, HandleRequestTargetNotFound)
{
    ProcessRegistry reg;
    WatcherForwarder fwd(reg);

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
    WatcherForwarder fwd(reg);

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
    WatcherForwarder fwd(reg);
    EXPECT_NO_THROW(fwd.check_timeouts());
    EXPECT_EQ(fwd.pending_count(), 0u);
}
