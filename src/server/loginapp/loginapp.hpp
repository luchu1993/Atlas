#pragma once

#include "foundation/time.hpp"
#include "login_messages.hpp"
#include "server/entity_types.hpp"
#include "server/manager_app.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

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

    LoginApp(EventDispatcher& dispatcher, NetworkInterface& network);

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
        WaitingPrepare,
    };

    struct PendingLogin
    {
        uint32_t request_id{0};
        Channel* client_ch{nullptr};
        std::string username;
        PendingStage stage{PendingStage::WaitingAuth};
        DatabaseID dbid{kInvalidDBID};
        uint16_t type_id{0};
        SessionKey session_key;
        Address baseapp_internal_addr;
        Address baseapp_external_addr;
        TimePoint created_at{};
    };

    // ---- Message handlers ---------------------------------------------------
    void on_login_request(const Address& src, Channel* ch, const login::LoginRequest& msg);
    void on_auth_login_result(const Address& src, Channel* ch, const login::AuthLoginResult& msg);
    void on_allocate_baseapp_result(const Address& src, Channel* ch,
                                    const login::AllocateBaseAppResult& msg);
    void on_prepare_login_result(const Address& src, Channel* ch,
                                 const login::PrepareLoginResult& msg);

    // ---- Internal helpers ---------------------------------------------------
    void send_login_error(Channel* ch, login::LoginStatus status, const std::string& msg);
    void cleanup_expired_logins();

    [[nodiscard]] auto is_rate_limited(const Address& src) -> bool;
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

    // ---- Config knobs -------------------------------------------------------
    static constexpr int kPerIpMaxPerWindow = 5;
    static constexpr int kGlobalMaxPerWindow = 1000;
    static constexpr std::chrono::seconds kRateWindow{60};
    static constexpr std::chrono::seconds kPendingTimeout{30};

    // ---- Login tracking ----------------------------------------------------
    std::unordered_map<uint32_t, PendingLogin> pending_;
    uint32_t next_request_id_{1};

    // ---- Connections --------------------------------------------------------
    Channel* dbapp_channel_{nullptr};
    Channel* baseappmgr_channel_{nullptr};
};

}  // namespace atlas
