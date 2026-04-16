#ifndef ATLAS_SERVER_LOGINAPP_LOGINAPP_H_
#define ATLAS_SERVER_LOGINAPP_LOGINAPP_H_

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "coro/cancellation.h"
#include "coro/fire_and_forget.h"
#include "coro/pending_rpc_registry.h"
#include "foundation/clock.h"
#include "login_messages.h"
#include "server/entity_types.h"
#include "server/ipv4_networks.h"
#include "server/manager_app.h"

namespace atlas {

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

class LoginApp : public ManagerApp {
 public:
  static auto Run(int argc, char* argv[]) -> int;

  LoginApp(EventDispatcher& dispatcher, NetworkInterface& network,
           NetworkInterface& external_network);

 protected:
  [[nodiscard]] auto Init(int argc, char* argv[]) -> bool override;
  void Fini() override;
  void OnTickComplete() override;
  void RegisterWatchers() override;

 private:
  friend class LoginRollbackTest;

  // ---- Message handlers ---------------------------------------------------
  void OnLoginRequest(const Address& src, Channel* ch, const login::LoginRequest& msg);

  // ---- Coroutine login flow -----------------------------------------------
  auto HandleLoginCoro(uint64_t client_channel_id, Address client_addr, login::LoginRequest request)
      -> FireAndForget;

  // ---- Internal helpers ---------------------------------------------------
  void OnClientDisconnect(Channel& ch);
  void SendLoginError(uint64_t client_channel_id, login::LoginStatus status,
                      const std::string& msg);
  void CleanupStaleRateEntries(TimePoint now);

  [[nodiscard]] auto IsRateLimited(const Address& src) -> bool;
  [[nodiscard]] auto IsTrustedRateLimitSource(const Address& src) const -> bool;
  void RecordLoginAttempt(const Address& src);

  // ---- Rate limiting state ------------------------------------------------
  struct RateEntry {
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

#endif  // ATLAS_SERVER_LOGINAPP_LOGINAPP_H_
