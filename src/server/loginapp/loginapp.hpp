#pragma once

#include "coro/cancellation.hpp"
#include "coro/fire_and_forget.hpp"
#include "coro/pending_rpc_registry.hpp"
#include "foundation/time.hpp"
#include "login_messages.hpp"
#include "server/entity_types.hpp"
#include "server/ipv4_networks.hpp"
#include "server/manager_app.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace atlas
{

class Channel;

// ============================================================================
// LoginApp — client authentication and login orchestration
//
// Login flow (coroutine-based):
//   1. Client connects → LoginRequest
//   2. co_await rpc_call<AuthLoginResult>     → DBApp auth
//   3. co_await rpc_call<AllocateBaseAppResult> → BaseAppMgr picks BaseApp
//   4. co_await rpc_call<PrepareLoginResult>  → BaseApp creates Proxy entity
//   5. LoginApp → Client: LoginResult (external_addr + session_key)
//
// Rollback: ScopeGuard + CancellationToken ensure cleanup on any failure.
// ============================================================================

class LoginApp : public ManagerApp
{
public:
    static auto run(int argc, char* argv[]) -> int;

    LoginApp(EventDispatcher& dispatcher, NetworkInterface& network,
             NetworkInterface& external_network);

protected:
    [[nodiscard]] auto init(int argc, char* argv[]) -> bool override;
    void fini() override;
    void on_tick_complete() override;
    void register_watchers() override;

private:
    friend class LoginRollbackTest;

    // ---- Message handlers ---------------------------------------------------
    void on_login_request(const Address& src, Channel* ch, const login::LoginRequest& msg);

    // ---- Coroutine login flow -----------------------------------------------
    auto handle_login_coro(uint64_t client_channel_id, Address client_addr,
                           login::LoginRequest request) -> FireAndForget;

    // ---- Internal helpers ---------------------------------------------------
    void on_client_disconnect(Channel& ch);
    void send_login_error(uint64_t client_channel_id, login::LoginStatus status,
                          const std::string& msg);
    void cleanup_stale_rate_entries(TimePoint now);

    [[nodiscard]] auto is_rate_limited(const Address& src) -> bool;
    [[nodiscard]] auto is_trusted_rate_limit_source(const Address& src) const -> bool;
    void record_login_attempt(const Address& src);

    // ---- Rate limiting state ------------------------------------------------
    struct RateEntry
    {
        int count{0};
        TimePoint window_start{};
    };
    std::unordered_map<uint32_t, RateEntry> rate_table_;  // key = client IP
    int global_login_count_{0};
    TimePoint global_window_start_{};
    TimePoint last_rate_cleanup_{};
    IPv4NetworkSet trusted_rate_limit_sources_;
    int rate_limit_per_ip_{5};
    int rate_limit_global_{1000};
    Duration rate_limit_window_{std::chrono::seconds(60)};

    // ---- Config knobs -------------------------------------------------------
    static constexpr std::chrono::seconds kRateCleanupInterval{60};
    static constexpr int kClientChannelInactivitySec = 3;
    static constexpr std::size_t kMaxPendingLogins = 2000;
    static constexpr auto kRpcTimeout = Milliseconds(10000);

    // ---- Coroutine RPC infrastructure ---------------------------------------
    PendingRpcRegistry rpc_registry_;

    // ---- Login tracking ----------------------------------------------------
    std::unordered_map<std::string, uint32_t> pending_by_username_;
    std::unordered_map<uint64_t, CancellationSource> channel_cancel_sources_;
    uint32_t next_request_id_{1};

    // ---- Connections --------------------------------------------------------
    NetworkInterface& external_network_;
    Channel* dbapp_channel_{nullptr};
    Channel* baseappmgr_channel_{nullptr};

    // ---- Metrics -----------------------------------------------------------
    uint64_t login_requests_total_{0};
    uint64_t login_success_total_{0};
    uint64_t login_fail_total_{0};
    uint64_t login_timeout_total_{0};
    uint64_t login_rate_limited_total_{0};
    uint64_t login_dedup_total_{0};
    uint64_t login_busy_total_{0};
    uint64_t abandoned_login_total_{0};
};

}  // namespace atlas
