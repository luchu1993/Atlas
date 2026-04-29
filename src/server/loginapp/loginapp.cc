#include "loginapp.h"

#include <format>

#include "coro/rpc_call.h"
#include "coro/scope_guard.h"
#include "foundation/log.h"
#include "network/channel.h"
#include "network/machined_types.h"
#include "network/reliable_udp.h"
#include "server/watcher.h"

namespace atlas {

// ============================================================================
// run — static entry point
// ============================================================================

auto LoginApp::Run(int argc, char* argv[]) -> int {
  EventDispatcher dispatcher;
  NetworkInterface internal_network(dispatcher);
  NetworkInterface external_network(dispatcher);
  LoginApp app(dispatcher, internal_network, external_network);
  return app.RunApp(argc, argv);
}

LoginApp::LoginApp(EventDispatcher& dispatcher, NetworkInterface& network,
                   NetworkInterface& external_network)
    : ManagerApp(dispatcher, network),
      rpc_registry_(dispatcher),
      external_network_(external_network) {}

// ============================================================================
// init
// ============================================================================

auto LoginApp::Init(int argc, char* argv[]) -> bool {
  if (!ManagerApp::Init(argc, argv)) return false;

  const auto& cfg = Config();
  if (cfg.login_rate_limit_per_ip < 0 || cfg.login_rate_limit_global < 0 ||
      cfg.login_rate_limit_window_sec <= 0) {
    ATLAS_LOG_ERROR("LoginApp: invalid rate-limit config per_ip={} global={} window_sec={}",
                    cfg.login_rate_limit_per_ip, cfg.login_rate_limit_global,
                    cfg.login_rate_limit_window_sec);
    return false;
  }

  rate_limit_per_ip_ = cfg.login_rate_limit_per_ip;
  rate_limit_global_ = cfg.login_rate_limit_global;
  rate_limit_window_ = std::chrono::seconds(cfg.login_rate_limit_window_sec);

  auto trusted_load = trusted_rate_limit_sources_.AddAll(cfg.login_rate_limit_trusted_cidrs);
  if (!trusted_load) {
    ATLAS_LOG_ERROR("LoginApp: invalid trusted rate-limit CIDR config: {}",
                    trusted_load.Error().Message());
    return false;
  }

  auto& client_table = external_network_.InterfaceTable();
  (void)client_table.RegisterTypedHandler<login::LoginRequest>(
      [this](const Address& src, Channel* ch, const login::LoginRequest& msg) {
        OnLoginRequest(src, ch, msg);
      });

  // Register RPC reply message descriptors so Channel knows their wire format
  // (especially fixed-length messages). The pre-dispatch hook intercepts them
  // before these no-op handlers run.
  auto& table = Network().InterfaceTable();
  (void)table.RegisterTypedHandler<login::AuthLoginResult>(
      [](const Address&, Channel*, const login::AuthLoginResult&) {});
  (void)table.RegisterTypedHandler<login::AllocateBaseAppResult>(
      [](const Address&, Channel*, const login::AllocateBaseAppResult&) {});
  (void)table.RegisterTypedHandler<login::PrepareLoginResult>(
      [](const Address&, Channel*, const login::PrepareLoginResult&) {});

  // Register pre-dispatch hook on internal network so rpc_call replies
  // are routed to the PendingRpcRegistry before normal handler dispatch.
  table.SetPreDispatchHook([this](MessageID id, std::span<const std::byte> payload) -> bool {
    return rpc_registry_.TryDispatch(id, payload);
  });

  // ---- Subscribe to DBApp birth ----------------------------------------
  GetMachinedClient().Subscribe(
      machined::ListenerType::kBoth, ProcessType::kDbApp,
      [this](const machined::BirthNotification& n) {
        if (dbapp_channel_ == nullptr) {
          ATLAS_LOG_INFO("LoginApp: DBApp born at {}:{}, connecting via RUDP...",
                         n.internal_addr.Ip(), n.internal_addr.Port());
          auto ch = Network().ConnectRudpNocwnd(n.internal_addr);
          if (ch) dbapp_channel_ = static_cast<Channel*>(*ch);
        }
      },
      [this](const machined::DeathNotification& /*n*/) {
        ATLAS_LOG_WARNING("LoginApp: DBApp died");
        dbapp_channel_ = nullptr;
      });

  // ---- Subscribe to BaseAppMgr birth ------------------------------------
  GetMachinedClient().Subscribe(
      machined::ListenerType::kBoth, ProcessType::kBaseAppMgr,
      [this](const machined::BirthNotification& n) {
        if (baseappmgr_channel_ == nullptr) {
          ATLAS_LOG_INFO("LoginApp: BaseAppMgr born at {}:{}, connecting via RUDP...",
                         n.internal_addr.Ip(), n.internal_addr.Port());
          auto ch = Network().ConnectRudpNocwnd(n.internal_addr);
          if (ch) baseappmgr_channel_ = static_cast<Channel*>(*ch);
        }
      },
      [this](const machined::DeathNotification& /*n*/) {
        ATLAS_LOG_WARNING("LoginApp: BaseAppMgr died");
        baseappmgr_channel_ = nullptr;
      });

  // Start RUDP listener for clients
  if (cfg.external_port > 0) {
    Address listen_addr(0, cfg.external_port);
    auto result =
        external_network_.StartRudpServer(listen_addr, NetworkInterface::InternetRudpProfile());
    if (!result) {
      ATLAS_LOG_ERROR("LoginApp: failed to listen on port {}: {}", cfg.external_port,
                      result.Error().Message());
      return false;
    }

    external_network_.SetAcceptCallback([](Channel& ch) {
      ch.SetInactivityTimeout(std::chrono::seconds(kClientChannelInactivitySec));
    });
    external_network_.SetDisconnectCallback([this](Channel& ch) { OnClientDisconnect(ch); });

    ATLAS_LOG_INFO("LoginApp: RUDP listener on port {}", cfg.external_port);
  }

  ATLAS_LOG_INFO(
      "LoginApp: login rate limit configured per_ip={} global={} window={}s trusted_cidrs={}",
      rate_limit_per_ip_, rate_limit_global_, cfg.login_rate_limit_window_sec,
      trusted_rate_limit_sources_.size());
  ATLAS_LOG_INFO("LoginApp: initialised");
  return true;
}

void LoginApp::Fini() {
  rpc_registry_.CancelAll();
  ManagerApp::Fini();
}

void LoginApp::OnTickComplete() {
  ManagerApp::OnTickComplete();
}

void LoginApp::FlushTickDirtyChannels() {
  ServerApp::FlushTickDirtyChannels();
  external_network_.FlushDirtySendChannels();
}

// ============================================================================
// register_watchers
// ============================================================================

void LoginApp::RegisterWatchers() {
  ManagerApp::RegisterWatchers();
  auto& wr = GetWatcherRegistry();
  wr.Add<uint64_t>("loginapp/login_requests_total",
                   std::function<uint64_t()>([this] { return login_requests_total_; }));
  wr.Add<uint64_t>("loginapp/login_success_total",
                   std::function<uint64_t()>([this] { return login_success_total_; }));
  wr.Add<uint64_t>("loginapp/login_fail_total",
                   std::function<uint64_t()>([this] { return login_fail_total_; }));
  wr.Add<uint64_t>("loginapp/login_timeout_total",
                   std::function<uint64_t()>([this] { return login_timeout_total_; }));
  wr.Add<uint64_t>("loginapp/login_rate_limited_total",
                   std::function<uint64_t()>([this] { return login_rate_limited_total_; }));
  wr.Add<uint64_t>("loginapp/login_dedup_total",
                   std::function<uint64_t()>([this] { return login_dedup_total_; }));
  wr.Add<uint64_t>("loginapp/login_busy_total",
                   std::function<uint64_t()>([this] { return login_busy_total_; }));
  wr.Add<uint64_t>("loginapp/abandoned_login_total",
                   std::function<uint64_t()>([this] { return abandoned_login_total_; }));
  wr.Add<int>("loginapp/pending_logins", std::function<int()>([this] {
                return static_cast<int>(pending_by_username_.size());
              }));
  wr.Add<bool>("loginapp/dbapp_connected",
               std::function<bool()>([this] { return dbapp_channel_ != nullptr; }));
  wr.Add<bool>("loginapp/baseappmgr_connected",
               std::function<bool()>([this] { return baseappmgr_channel_ != nullptr; }));
}

// ============================================================================
// on_login_request — step 1: client sends credentials
// ============================================================================

void LoginApp::OnLoginRequest(const Address& src, Channel* ch, const login::LoginRequest& msg) {
  if (ch == nullptr) {
    return;
  }

  ++login_requests_total_;
  ATLAS_LOG_DEBUG("LoginApp: login request user='{}' from {}:{}", msg.username, src.Ip(),
                  src.Port());

  if (IsRateLimited(src)) {
    ++login_rate_limited_total_;
    ATLAS_LOG_DEBUG("LoginApp: rate-limited login from {}:{}", src.Ip(), src.Port());
    SendLoginError(ch->ChannelId(), login::LoginStatus::kRateLimited, "too many attempts");
    return;
  }
  RecordLoginAttempt(src);

  // Reject if this channel already has a login in progress
  if (channel_cancel_sources_.contains(ch->ChannelId())) {
    ++login_dedup_total_;
    ATLAS_LOG_DEBUG("LoginApp: duplicate login from same channel {}:{}", src.Ip(), src.Port());
    SendLoginError(ch->ChannelId(), login::LoginStatus::kLoginInProgress, "login_in_progress");
    return;
  }

  if (pending_by_username_.contains(msg.username)) {
    ++login_dedup_total_;
    ATLAS_LOG_DEBUG("LoginApp: dedup login for '{}' from {}:{}", msg.username, src.Ip(),
                    src.Port());
    SendLoginError(ch->ChannelId(), login::LoginStatus::kLoginInProgress, "login_in_progress");
    return;
  }

  if (pending_by_username_.size() >= kMaxPendingLogins) {
    ++login_busy_total_;
    ATLAS_LOG_DEBUG("LoginApp: admission control, pending={} from {}:{}",
                    pending_by_username_.size(), src.Ip(), src.Port());
    SendLoginError(ch->ChannelId(), login::LoginStatus::kServerBusy, "server_busy");
    return;
  }

  if (!dbapp_channel_) {
    ATLAS_LOG_ERROR("LoginApp: no DBApp connection for login request");
    SendLoginError(ch->ChannelId(), login::LoginStatus::kServerNotReady, "no_dbapp");
    return;
  }

  if (!baseappmgr_channel_) {
    ATLAS_LOG_ERROR("LoginApp: no BaseAppMgr connection for login request");
    SendLoginError(ch->ChannelId(), login::LoginStatus::kServerNotReady, "no_baseappmgr");
    return;
  }

  HandleLoginCoro(ch->ChannelId(), src, msg);
}

// ============================================================================
// handle_login_coro — coroutine: orchestrates the entire login flow
// ============================================================================

auto LoginApp::HandleLoginCoro(uint64_t client_channel_id, Address client_addr,
                               login::LoginRequest request) -> FireAndForget {
  uint32_t rid = next_request_id_++;
  std::string username = request.username;

  // ---- Dedup guard: remove from pending_by_username_ on any exit path ----
  pending_by_username_[username] = rid;
  ScopeGuard dedup_guard([this, username] { pending_by_username_.erase(username); });

  // ---- Cancellation: cancelled when client disconnects --------------------
  CancellationSource cancel_source;
  auto token = cancel_source.Token();
  channel_cancel_sources_[client_channel_id] = cancel_source;
  ScopeGuard cancel_guard(
      [this, client_channel_id] { channel_cancel_sources_.erase(client_channel_id); });

  // ======================================================================
  // Step 1: Authenticate with DBApp
  // ======================================================================
  if (!dbapp_channel_) {
    SendLoginError(client_channel_id, login::LoginStatus::kServerNotReady, "no_dbapp");
    co_return;
  }

  login::AuthLogin auth;
  auth.request_id = rid;
  auth.username = request.username;
  auth.password_hash = request.password_hash;
  auth.auto_create = Config().auto_create_accounts;

  auto auth_result = co_await RpcCall<login::AuthLoginResult>(rpc_registry_, *dbapp_channel_, auth,
                                                              kRpcTimeout, token);
  if (!auth_result) {
    if (auth_result.Error().Code() == ErrorCode::kCancelled) {
      ++abandoned_login_total_;
      ATLAS_LOG_DEBUG("LoginApp: login cancelled for '{}' (client disconnected)", username);
      co_return;
    }
    if (auth_result.Error().Code() == ErrorCode::kTimeout) ++login_timeout_total_;
    ATLAS_LOG_WARNING("LoginApp: auth RPC failed for '{}': {}", username,
                      auth_result.Error().Message());
    SendLoginError(client_channel_id, login::LoginStatus::kInternalError, "auth_rpc_failed");
    co_return;
  }

  const auto& auth_reply = auth_result.Value();
  ATLAS_LOG_DEBUG("LoginApp: auth result request_id={} success={} status={}", rid,
                  auth_reply.success, static_cast<int>(auth_reply.status));

  if (!auth_reply.success) {
    ATLAS_LOG_DEBUG("LoginApp: auth failed for '{}' status={}", username,
                    static_cast<int>(auth_reply.status));
    SendLoginError(client_channel_id, auth_reply.status, "auth_failed");
    co_return;
  }

  // ======================================================================
  // Step 2: Allocate a BaseApp via BaseAppMgr
  // ======================================================================
  if (!baseappmgr_channel_) {
    ATLAS_LOG_ERROR("LoginApp: no BaseAppMgr connection");
    SendLoginError(client_channel_id, login::LoginStatus::kServerNotReady, "no_baseappmgr");
    co_return;
  }

  login::AllocateBaseApp alloc;
  alloc.request_id = rid;
  alloc.type_id = auth_reply.type_id;
  alloc.dbid = auth_reply.dbid;

  auto alloc_result = co_await RpcCall<login::AllocateBaseAppResult>(
      rpc_registry_, *baseappmgr_channel_, alloc, kRpcTimeout, token);
  if (!alloc_result) {
    if (alloc_result.Error().Code() == ErrorCode::kCancelled) {
      ++abandoned_login_total_;
      ATLAS_LOG_DEBUG("LoginApp: login cancelled for '{}' (client disconnected)", username);
      co_return;
    }
    if (alloc_result.Error().Code() == ErrorCode::kTimeout) ++login_timeout_total_;
    ATLAS_LOG_WARNING("LoginApp: allocate baseapp RPC failed for '{}': {}", username,
                      alloc_result.Error().Message());
    SendLoginError(client_channel_id, login::LoginStatus::kInternalError, "allocate_rpc_failed");
    co_return;
  }

  const auto& alloc_reply = alloc_result.Value();
  ATLAS_LOG_DEBUG("LoginApp: allocate baseapp result request_id={} success={} internal={}:{}", rid,
                  alloc_reply.success, alloc_reply.internal_addr.Ip(),
                  alloc_reply.internal_addr.Port());

  if (!alloc_reply.success) {
    ATLAS_LOG_WARNING("LoginApp: no BaseApp available for '{}'", username);
    SendLoginError(client_channel_id, login::LoginStatus::kServerFull, "no_baseapp");
    co_return;
  }

  // ======================================================================
  // Step 3: PrepareLogin on the allocated BaseApp
  // ======================================================================
  auto ch_result = Network().ConnectRudpNocwnd(alloc_reply.internal_addr);
  if (!ch_result) {
    ATLAS_LOG_ERROR("LoginApp: could not connect to BaseApp {}:{}", alloc_reply.internal_addr.Ip(),
                    alloc_reply.internal_addr.Port());
    SendLoginError(client_channel_id, login::LoginStatus::kInternalError, "connect_failed");
    co_return;
  }
  Channel* baseapp_ch = static_cast<Channel*>(*ch_result);

  auto session_key = SessionKey::Generate();

  login::PrepareLogin prep;
  prep.request_id = rid;
  prep.type_id = auth_reply.type_id;
  prep.dbid = auth_reply.dbid;
  prep.session_key = session_key;
  prep.client_addr = client_addr;

  // ---- Rollback guard: send CancelPrepareLogin if we fail after this point
  ScopeGuard prepare_guard(
      [this, rid, dbid = auth_reply.dbid, baseapp_addr = alloc_reply.internal_addr] {
        auto cancel_ch = Network().ConnectRudpNocwnd(baseapp_addr);
        if (cancel_ch) {
          login::CancelPrepareLogin cancel;
          cancel.request_id = rid;
          cancel.dbid = dbid;
          if (auto r = (*cancel_ch)->SendMessage(cancel); !r) {
            // Cleanup best-effort, but BaseApp leaks pending entry until
            // its kPendingTimeout expires.  Warn so leaked entries don't
            // look like a slow-cleanup bug.
            ATLAS_LOG_WARNING(
                "LoginApp: CancelPrepareLogin send failed (request_id={}, dbid={}, baseapp {}): {}",
                rid, dbid, baseapp_addr.ToString(), r.Error().Message());
          }
        } else {
          ATLAS_LOG_WARNING(
              "LoginApp: failed to send CancelPrepareLogin request_id={} to "
              "{}:{}",
              rid, baseapp_addr.Ip(), baseapp_addr.Port());
        }
      });

  auto prep_result = co_await RpcCall<login::PrepareLoginResult>(rpc_registry_, *baseapp_ch, prep,
                                                                 kRpcTimeout, token);
  if (!prep_result) {
    if (prep_result.Error().Code() == ErrorCode::kCancelled) {
      ++abandoned_login_total_;
      ATLAS_LOG_DEBUG("LoginApp: login cancelled for '{}' (client disconnected)", username);
      co_return;  // prepare_guard fires → CancelPrepareLogin sent
    }
    if (prep_result.Error().Code() == ErrorCode::kTimeout) ++login_timeout_total_;
    ATLAS_LOG_WARNING("LoginApp: prepare login RPC failed for '{}': {}", username,
                      prep_result.Error().Message());
    SendLoginError(client_channel_id, login::LoginStatus::kInternalError, "prepare_rpc_failed");
    co_return;  // prepare_guard fires → CancelPrepareLogin sent
  }

  const auto& prep_reply = prep_result.Value();
  ATLAS_LOG_DEBUG("LoginApp: prepare login result request_id={} success={} entity_id={} error='{}'",
                  rid, prep_reply.success, prep_reply.entity_id, prep_reply.error);

  if (!prep_reply.success) {
    ATLAS_LOG_ERROR("LoginApp: PrepareLogin failed for '{}': {}", username, prep_reply.error);
    SendLoginError(client_channel_id, login::LoginStatus::kInternalError, prep_reply.error);
    co_return;  // prepare_guard fires → CancelPrepareLogin sent
  }

  // ======================================================================
  // Success — dismiss rollback guard and send LoginResult to client
  // ======================================================================
  prepare_guard.Dismiss();

  auto* client_ch = external_network_.FindChannel(client_channel_id);
  if (!client_ch) {
    ATLAS_LOG_WARNING("LoginApp: client channel disappeared before login completion request_id={}",
                      rid);
    ++abandoned_login_total_;
    co_return;
  }

  login::LoginResult result;
  result.status = login::LoginStatus::kSuccess;
  result.session_key = session_key;
  result.baseapp_addr = alloc_reply.external_addr;

  if (!client_ch->SendMessage(result)) {
    ATLAS_LOG_WARNING("LoginApp: failed to deliver LoginResult request_id={}", rid);
    ++abandoned_login_total_;
    co_return;
  }

  ++login_success_total_;
  ATLAS_LOG_DEBUG("LoginApp: login complete for '{}' entity={} baseapp={}:{}", username,
                  prep_reply.entity_id, alloc_reply.external_addr.Ip(),
                  alloc_reply.external_addr.Port());
}

// ============================================================================
// Helpers
// ============================================================================

void LoginApp::OnClientDisconnect(Channel& ch) {
  auto it = channel_cancel_sources_.find(ch.ChannelId());
  if (it != channel_cancel_sources_.end()) {
    it->second.RequestCancellation();
    // Note: the coroutine's ScopeGuard removes the entry from channel_cancel_sources_
    // when it finishes, so we don't erase here — the cancellation callback resumes
    // the coroutine synchronously, which erases the entry via cancel_guard.
  }
}

void LoginApp::SendLoginError(uint64_t client_channel_id, login::LoginStatus status,
                              const std::string& msg) {
  ++login_fail_total_;
  auto* ch = external_network_.FindChannel(client_channel_id);
  if (!ch) return;
  login::LoginResult result;
  result.status = status;
  result.error_message = msg;
  (void)ch->SendMessage(result);
}

auto LoginApp::IsRateLimited(const Address& src) -> bool {
  const auto kNow = Clock::now();
  CleanupStaleRateEntries(kNow);

  if (rate_limit_per_ip_ <= 0 && rate_limit_global_ <= 0) {
    return false;
  }

  if (IsTrustedRateLimitSource(src)) {
    return false;
  }

  // Global window check
  if (global_window_start_ == TimePoint{}) global_window_start_ = kNow;
  if (kNow - global_window_start_ > rate_limit_window_) {
    global_window_start_ = kNow;
    global_login_count_ = 0;
  }
  if (rate_limit_global_ > 0 && global_login_count_ >= rate_limit_global_) return true;

  // Per-IP window check
  if (rate_limit_per_ip_ <= 0) {
    return false;
  }

  auto& entry = rate_table_[src.Ip()];
  if (entry.window_start == TimePoint{}) entry.window_start = kNow;
  if (kNow - entry.window_start > rate_limit_window_) {
    entry.window_start = kNow;
    entry.count = 0;
  }
  return entry.count >= rate_limit_per_ip_;
}

auto LoginApp::IsTrustedRateLimitSource(const Address& src) const -> bool {
  return !trusted_rate_limit_sources_.empty() && trusted_rate_limit_sources_.contains(src.Ip());
}

void LoginApp::RecordLoginAttempt(const Address& src) {
  if (IsTrustedRateLimitSource(src)) {
    return;
  }

  if (rate_limit_global_ > 0) {
    ++global_login_count_;
  }
  if (rate_limit_per_ip_ > 0) {
    auto& entry = rate_table_[src.Ip()];
    if (entry.window_start == TimePoint{}) {
      entry.window_start = Clock::now();
    }
    ++entry.count;
  }
}

void LoginApp::CleanupStaleRateEntries(TimePoint now) {
  if (now - last_rate_cleanup_ < kRateCleanupInterval) {
    return;
  }
  last_rate_cleanup_ = now;

  for (auto it = rate_table_.begin(); it != rate_table_.end();) {
    if (now - it->second.window_start > rate_limit_window_) {
      it = rate_table_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace atlas
