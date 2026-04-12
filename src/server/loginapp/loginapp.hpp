#pragma once

#include "dbapp/dbapp_messages.hpp"
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
// Login flow:
//   1. Client connects → LoginRequest
//   2. LoginApp → DBApp: AuthLogin (validate credentials)
//   3. DBApp → LoginApp: AuthLoginResult
//   4. LoginApp → BaseAppMgr: AllocateBaseApp
//   5. BaseAppMgr → LoginApp: AllocateBaseAppResult
//   6. LoginApp → BaseApp: PrepareLogin (creates Proxy entity)
//   7. BaseApp → LoginApp: PrepareLoginResult (entity_id + ready)
//   8. LoginApp → Client: LoginResult (external_addr + session_key)
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
    // ---- Pending login state machine ----------------------------------------
    enum class PendingStage : uint8_t
    {
        WaitingAuth = 0,
        WaitingBaseApp,
        WaitingCheckout,
        WaitingPrepare,
    };

    struct PendingLogin
    {
        uint32_t request_id{0};
        uint64_t client_channel_id{0};
        Address client_addr;
        std::string username;
        PendingStage stage{PendingStage::WaitingAuth};
        DatabaseID dbid{kInvalidDBID};
        uint16_t type_id{0};
        SessionKey session_key;
        Address baseapp_internal_addr;
        Address baseapp_external_addr;
        TimePoint created_at{};
        std::vector<std::byte> entity_blob;
    };

    // ---- Message handlers ---------------------------------------------------
    void on_login_request(const Address& src, Channel* ch, const login::LoginRequest& msg);
    void on_auth_login_result(const Address& src, Channel* ch, const login::AuthLoginResult& msg);
    void on_allocate_baseapp_result(const Address& src, Channel* ch,
                                    const login::AllocateBaseAppResult& msg);
    void on_prepare_login_result(const Address& src, Channel* ch,
                                 const login::PrepareLoginResult& msg);
    void on_checkout_entity_ack(const Address& src, Channel* ch,
                                const dbapp::CheckoutEntityAck& msg);

    // ---- Internal helpers ---------------------------------------------------
    void on_client_disconnect(Channel& ch);
    void cancel_prepare_login(const PendingLogin& pending);
    void abandon_pending_login(std::unordered_map<uint32_t, PendingLogin>::iterator it);
    void send_login_error(uint64_t client_channel_id, login::LoginStatus status,
                          const std::string& msg);
    void remove_pending(std::unordered_map<uint32_t, PendingLogin>::iterator it);
    void cleanup_expired_logins();
    void cleanup_stale_rate_entries(TimePoint now);
    void cleanup_canceled_requests(TimePoint now);

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
    static constexpr std::chrono::seconds kPendingTimeout{10};
    static constexpr std::chrono::seconds kRateCleanupInterval{60};
    static constexpr std::chrono::seconds kCanceledRequestRetention{10};
    static constexpr int kClientChannelInactivitySec = 3;
    static constexpr std::size_t kMaxPendingLogins = 2000;

    // ---- Login tracking ----------------------------------------------------
    std::unordered_map<uint32_t, PendingLogin> pending_;
    std::unordered_map<std::string, uint32_t> pending_by_username_;
    std::unordered_map<uint32_t, TimePoint> canceled_requests_;
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
};

}  // namespace atlas
