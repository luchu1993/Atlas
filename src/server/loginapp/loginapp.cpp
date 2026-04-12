#include "loginapp.hpp"

#include "baseappmgr/baseappmgr_messages.hpp"
#include "dbapp/dbapp_messages.hpp"
#include "foundation/log.hpp"
#include "network/channel.hpp"
#include "network/machined_types.hpp"
#include "network/reliable_udp.hpp"
#include "server/watcher.hpp"

#include <algorithm>
#include <format>

namespace atlas
{

// ============================================================================
// run — static entry point
// ============================================================================

auto LoginApp::run(int argc, char* argv[]) -> int
{
    EventDispatcher dispatcher;
    NetworkInterface internal_network(dispatcher);
    NetworkInterface external_network(dispatcher);
    LoginApp app(dispatcher, internal_network, external_network);
    return app.run_app(argc, argv);
}

LoginApp::LoginApp(EventDispatcher& dispatcher, NetworkInterface& network,
                   NetworkInterface& external_network)
    : ManagerApp(dispatcher, network), external_network_(external_network)
{
}

// ============================================================================
// init
// ============================================================================

auto LoginApp::init(int argc, char* argv[]) -> bool
{
    if (!ManagerApp::init(argc, argv))
        return false;

    const auto& cfg = config();
    if (cfg.login_rate_limit_per_ip < 0 || cfg.login_rate_limit_global < 0 ||
        cfg.login_rate_limit_window_sec <= 0)
    {
        ATLAS_LOG_ERROR("LoginApp: invalid rate-limit config per_ip={} global={} window_sec={}",
                        cfg.login_rate_limit_per_ip, cfg.login_rate_limit_global,
                        cfg.login_rate_limit_window_sec);
        return false;
    }

    rate_limit_per_ip_ = cfg.login_rate_limit_per_ip;
    rate_limit_global_ = cfg.login_rate_limit_global;
    rate_limit_window_ = std::chrono::seconds(cfg.login_rate_limit_window_sec);

    auto trusted_load = trusted_rate_limit_sources_.add_all(cfg.login_rate_limit_trusted_cidrs);
    if (!trusted_load)
    {
        ATLAS_LOG_ERROR("LoginApp: invalid trusted rate-limit CIDR config: {}",
                        trusted_load.error().message());
        return false;
    }

    auto& client_table = external_network_.interface_table();
    (void)client_table.register_typed_handler<login::LoginRequest>(
        [this](const Address& src, Channel* ch, const login::LoginRequest& msg)
        { on_login_request(src, ch, msg); });

    auto& table = network().interface_table();

    // DBApp responses
    (void)table.register_typed_handler<login::AuthLoginResult>(
        [this](const Address& src, Channel* ch, const login::AuthLoginResult& msg)
        { on_auth_login_result(src, ch, msg); });

    // BaseAppMgr responses
    (void)table.register_typed_handler<login::AllocateBaseAppResult>(
        [this](const Address& src, Channel* ch, const login::AllocateBaseAppResult& msg)
        { on_allocate_baseapp_result(src, ch, msg); });

    // BaseApp responses
    (void)table.register_typed_handler<login::PrepareLoginResult>(
        [this](const Address& src, Channel* ch, const login::PrepareLoginResult& msg)
        { on_prepare_login_result(src, ch, msg); });

    // DBApp checkout responses (for LoginApp-proxy checkout)
    (void)table.register_typed_handler<dbapp::CheckoutEntityAck>(
        [this](const Address& src, Channel* ch, const dbapp::CheckoutEntityAck& msg)
        { on_checkout_entity_ack(src, ch, msg); });

    // ---- Subscribe to DBApp birth ----------------------------------------
    machined_client().subscribe(
        machined::ListenerType::Both, ProcessType::DBApp,
        [this](const machined::BirthNotification& n)
        {
            if (dbapp_channel_ == nullptr)
            {
                ATLAS_LOG_INFO("LoginApp: DBApp born at {}:{}, connecting via RUDP...",
                               n.internal_addr.ip(), n.internal_addr.port());
                auto ch = network().connect_rudp_nocwnd(n.internal_addr);
                if (ch)
                    dbapp_channel_ = static_cast<Channel*>(*ch);
            }
        },
        [this](const machined::DeathNotification& /*n*/)
        {
            ATLAS_LOG_WARNING("LoginApp: DBApp died");
            dbapp_channel_ = nullptr;
        });

    // ---- Subscribe to BaseAppMgr birth ------------------------------------
    machined_client().subscribe(
        machined::ListenerType::Both, ProcessType::BaseAppMgr,
        [this](const machined::BirthNotification& n)
        {
            if (baseappmgr_channel_ == nullptr)
            {
                ATLAS_LOG_INFO("LoginApp: BaseAppMgr born at {}:{}, connecting via RUDP...",
                               n.internal_addr.ip(), n.internal_addr.port());
                auto ch = network().connect_rudp_nocwnd(n.internal_addr);
                if (ch)
                    baseappmgr_channel_ = static_cast<Channel*>(*ch);
            }
        },
        [this](const machined::DeathNotification& /*n*/)
        {
            ATLAS_LOG_WARNING("LoginApp: BaseAppMgr died");
            baseappmgr_channel_ = nullptr;
        });

    // Start RUDP listener for clients
    if (cfg.external_port > 0)
    {
        Address listen_addr(0, cfg.external_port);
        auto result = external_network_.start_rudp_server(
            listen_addr, NetworkInterface::internet_rudp_profile());
        if (!result)
        {
            ATLAS_LOG_ERROR("LoginApp: failed to listen on port {}: {}", cfg.external_port,
                            result.error().message());
            return false;
        }

        external_network_.set_accept_callback(
            [](Channel& ch)
            { ch.set_inactivity_timeout(std::chrono::seconds(kClientChannelInactivitySec)); });
        external_network_.set_disconnect_callback([this](Channel& ch)
                                                  { on_client_disconnect(ch); });

        ATLAS_LOG_INFO("LoginApp: RUDP listener on port {}", cfg.external_port);
    }

    ATLAS_LOG_INFO(
        "LoginApp: login rate limit configured per_ip={} global={} window={}s trusted_cidrs={}",
        rate_limit_per_ip_, rate_limit_global_, cfg.login_rate_limit_window_sec,
        trusted_rate_limit_sources_.size());
    ATLAS_LOG_INFO("LoginApp: initialised");
    return true;
}

void LoginApp::fini()
{
    ManagerApp::fini();
}

void LoginApp::on_tick_complete()
{
    cleanup_expired_logins();
    cleanup_canceled_requests(Clock::now());
    ManagerApp::on_tick_complete();
}

// ============================================================================
// register_watchers
// ============================================================================

void LoginApp::register_watchers()
{
    ManagerApp::register_watchers();
    auto& wr = watcher_registry();
    wr.add<uint64_t>("loginapp/login_requests_total",
                     std::function<uint64_t()>([this] { return login_requests_total_; }));
    wr.add<uint64_t>("loginapp/login_success_total",
                     std::function<uint64_t()>([this] { return login_success_total_; }));
    wr.add<uint64_t>("loginapp/login_fail_total",
                     std::function<uint64_t()>([this] { return login_fail_total_; }));
    wr.add<uint64_t>("loginapp/login_timeout_total",
                     std::function<uint64_t()>([this] { return login_timeout_total_; }));
    wr.add<uint64_t>("loginapp/login_rate_limited_total",
                     std::function<uint64_t()>([this] { return login_rate_limited_total_; }));
    wr.add<uint64_t>("loginapp/login_busy_total",
                     std::function<uint64_t()>([this] { return login_busy_total_; }));
    wr.add<int>("loginapp/pending_logins",
                std::function<int()>([this] { return static_cast<int>(pending_.size()); }));
    wr.add<bool>("loginapp/dbapp_connected",
                 std::function<bool()>([this] { return dbapp_channel_ != nullptr; }));
    wr.add<bool>("loginapp/baseappmgr_connected",
                 std::function<bool()>([this] { return baseappmgr_channel_ != nullptr; }));
}

// ============================================================================
// on_login_request — step 1: client sends credentials
// ============================================================================

void LoginApp::on_login_request(const Address& src, Channel* ch, const login::LoginRequest& msg)
{
    if (ch == nullptr)
    {
        return;
    }

    ++login_requests_total_;
    ATLAS_LOG_DEBUG("LoginApp: login request user='{}' from {}:{}", msg.username, src.ip(),
                    src.port());

    if (is_rate_limited(src))
    {
        ++login_rate_limited_total_;
        ATLAS_LOG_DEBUG("LoginApp: rate-limited login from {}:{}", src.ip(), src.port());
        send_login_error(ch->channel_id(), login::LoginStatus::RateLimited, "too many attempts");
        return;
    }
    record_login_attempt(src);

    if (pending_by_username_.contains(msg.username))
    {
        ++login_dedup_total_;
        ATLAS_LOG_DEBUG("LoginApp: dedup login for '{}' from {}:{}", msg.username, src.ip(),
                        src.port());
        send_login_error(ch->channel_id(), login::LoginStatus::LoginInProgress,
                         "login_in_progress");
        return;
    }

    if (pending_.size() >= kMaxPendingLogins)
    {
        ++login_busy_total_;
        ATLAS_LOG_DEBUG("LoginApp: admission control, pending={} from {}:{}", pending_.size(),
                        src.ip(), src.port());
        send_login_error(ch->channel_id(), login::LoginStatus::ServerBusy, "server_busy");
        return;
    }

    if (!dbapp_channel_)
    {
        ATLAS_LOG_ERROR("LoginApp: no DBApp connection for login request");
        send_login_error(ch->channel_id(), login::LoginStatus::ServerNotReady, "no_dbapp");
        return;
    }

    uint32_t rid = next_request_id_++;
    PendingLogin pending;
    pending.request_id = rid;
    pending.client_channel_id = ch->channel_id();
    pending.client_addr = src;
    pending.username = msg.username;
    pending.stage = PendingStage::WaitingAuth;
    pending.created_at = Clock::now();
    pending_by_username_[msg.username] = rid;
    pending_[rid] = std::move(pending);

    const auto& cfg = config();

    login::AuthLogin auth;
    auth.request_id = rid;
    auth.username = msg.username;
    auth.password_hash = msg.password_hash;
    auth.auto_create = cfg.auto_create_accounts;
    (void)dbapp_channel_->send_message(auth);
}

// ============================================================================
// on_auth_login_result — step 3: DBApp replies with auth outcome
// ============================================================================

void LoginApp::on_auth_login_result(const Address& /*src*/, Channel* /*ch*/,
                                    const login::AuthLoginResult& msg)
{
    auto it = pending_.find(msg.request_id);
    if (it == pending_.end())
    {
        if (canceled_requests_.contains(msg.request_id))
        {
            return;
        }
        ATLAS_LOG_WARNING("LoginApp: AuthLoginResult for unknown request_id={}", msg.request_id);
        return;
    }
    PendingLogin& pending = it->second;
    ATLAS_LOG_DEBUG("LoginApp: auth result request_id={} success={} status={}", msg.request_id,
                    msg.success, static_cast<int>(msg.status));

    if (!msg.success)
    {
        ATLAS_LOG_DEBUG("LoginApp: auth failed for '{}' status={}", pending.username,
                        static_cast<int>(msg.status));
        send_login_error(pending.client_channel_id, msg.status, "auth_failed");
        remove_pending(it);
        return;
    }

    if (!baseappmgr_channel_)
    {
        ATLAS_LOG_ERROR("LoginApp: no BaseAppMgr connection");
        send_login_error(pending.client_channel_id, login::LoginStatus::ServerNotReady,
                         "no_baseappmgr");
        remove_pending(it);
        return;
    }

    pending.dbid = msg.dbid;
    pending.type_id = msg.type_id;
    pending.stage = PendingStage::WaitingBaseApp;

    login::AllocateBaseApp alloc;
    alloc.request_id = msg.request_id;
    alloc.type_id = msg.type_id;
    alloc.dbid = msg.dbid;
    (void)baseappmgr_channel_->send_message(alloc);
}

// ============================================================================
// on_allocate_baseapp_result — step 5: BaseAppMgr picks a BaseApp
// ============================================================================

void LoginApp::on_allocate_baseapp_result(const Address& /*src*/, Channel* /*ch*/,
                                          const login::AllocateBaseAppResult& msg)
{
    auto it = pending_.find(msg.request_id);
    if (it == pending_.end())
    {
        if (canceled_requests_.contains(msg.request_id))
        {
            return;
        }
        ATLAS_LOG_WARNING("LoginApp: AllocateBaseAppResult for unknown request_id={}",
                          msg.request_id);
        return;
    }
    PendingLogin& pending = it->second;
    ATLAS_LOG_DEBUG("LoginApp: allocate baseapp result request_id={} success={} internal={}:{}",
                    msg.request_id, msg.success, msg.internal_addr.ip(), msg.internal_addr.port());

    if (!msg.success)
    {
        ATLAS_LOG_WARNING("LoginApp: no BaseApp available for '{}'", pending.username);
        send_login_error(pending.client_channel_id, login::LoginStatus::ServerFull, "no_baseapp");
        remove_pending(it);
        return;
    }

    pending.baseapp_internal_addr = msg.internal_addr;
    pending.baseapp_external_addr = msg.external_addr;
    pending.session_key = SessionKey::generate();
    pending.stage = PendingStage::WaitingPrepare;

    auto ch_result = network().connect_rudp_nocwnd(msg.internal_addr);
    if (!ch_result)
    {
        ATLAS_LOG_ERROR("LoginApp: could not connect to BaseApp {}:{}", msg.internal_addr.ip(),
                        msg.internal_addr.port());
        send_login_error(pending.client_channel_id, login::LoginStatus::InternalError,
                         "connect_failed");
        remove_pending(it);
        return;
    }
    Channel* baseapp_ch = static_cast<Channel*>(*ch_result);

    login::PrepareLogin prep;
    prep.request_id = msg.request_id;
    prep.type_id = pending.type_id;
    prep.dbid = pending.dbid;
    prep.session_key = pending.session_key;
    prep.client_addr = pending.client_addr;
    (void)baseapp_ch->send_message(prep);
}

// ============================================================================
// on_checkout_entity_ack — DBApp returns entity blob for proxy checkout
// ============================================================================

void LoginApp::on_checkout_entity_ack(const Address& /*src*/, Channel* /*ch*/,
                                      const dbapp::CheckoutEntityAck& msg)
{
    auto it = pending_.find(msg.request_id);
    if (it == pending_.end())
    {
        if (canceled_requests_.contains(msg.request_id))
        {
            return;
        }
        ATLAS_LOG_WARNING("LoginApp: CheckoutEntityAck for unknown request_id={}", msg.request_id);
        return;
    }
    PendingLogin& pending = it->second;

    if (msg.status != dbapp::CheckoutStatus::Success)
    {
        if (msg.status == dbapp::CheckoutStatus::AlreadyCheckedOut)
        {
            // Fall back: let BaseApp handle force-logoff via the old path (no blob)
            ATLAS_LOG_DEBUG("LoginApp: checkout conflict for '{}' dbid={}, delegating to BaseApp",
                            pending.username, pending.dbid);
            pending.stage = PendingStage::WaitingPrepare;

            auto ch_result = network().connect_rudp_nocwnd(pending.baseapp_internal_addr);
            if (!ch_result)
            {
                send_login_error(pending.client_channel_id, login::LoginStatus::InternalError,
                                 "connect_failed");
                remove_pending(it);
                return;
            }
            Channel* baseapp_ch = static_cast<Channel*>(*ch_result);

            login::PrepareLogin prep;
            prep.request_id = msg.request_id;
            prep.type_id = pending.type_id;
            prep.dbid = pending.dbid;
            prep.session_key = pending.session_key;
            prep.client_addr = pending.client_addr;
            (void)baseapp_ch->send_message(prep);
            return;
        }

        ATLAS_LOG_WARNING("LoginApp: checkout failed for '{}' dbid={} status={}", pending.username,
                          pending.dbid, static_cast<int>(msg.status));
        send_login_error(pending.client_channel_id, login::LoginStatus::InternalError,
                         msg.error.empty() ? "checkout_failed" : msg.error);
        remove_pending(it);
        return;
    }

    pending.entity_blob = msg.blob;
    pending.stage = PendingStage::WaitingPrepare;

    auto ch_result = network().connect_rudp_nocwnd(pending.baseapp_internal_addr);
    if (!ch_result)
    {
        ATLAS_LOG_ERROR("LoginApp: could not connect to BaseApp {}:{}",
                        pending.baseapp_internal_addr.ip(), pending.baseapp_internal_addr.port());
        send_login_error(pending.client_channel_id, login::LoginStatus::InternalError,
                         "connect_failed");
        remove_pending(it);
        return;
    }
    Channel* baseapp_ch = static_cast<Channel*>(*ch_result);

    login::PrepareLogin prep;
    prep.request_id = msg.request_id;
    prep.type_id = pending.type_id;
    prep.dbid = pending.dbid;
    prep.session_key = pending.session_key;
    prep.client_addr = pending.client_addr;
    prep.blob_prefetched = true;
    prep.entity_blob = std::move(pending.entity_blob);
    (void)baseapp_ch->send_message(prep);
}

// ============================================================================
// on_prepare_login_result — step 7: BaseApp created the Proxy
// ============================================================================

void LoginApp::on_prepare_login_result(const Address& /*src*/, Channel* /*ch*/,
                                       const login::PrepareLoginResult& msg)
{
    auto it = pending_.find(msg.request_id);
    if (it == pending_.end())
    {
        if (canceled_requests_.contains(msg.request_id))
        {
            return;
        }
        ATLAS_LOG_WARNING("LoginApp: PrepareLoginResult for unknown request_id={}", msg.request_id);
        return;
    }
    PendingLogin& pending = it->second;
    ATLAS_LOG_DEBUG(
        "LoginApp: prepare login result request_id={} success={} entity_id={} error='{}'",
        msg.request_id, msg.success, msg.entity_id, msg.error);

    if (!msg.success)
    {
        ATLAS_LOG_ERROR("LoginApp: PrepareLogin failed for '{}': {}", pending.username, msg.error);
        send_login_error(pending.client_channel_id, login::LoginStatus::InternalError, msg.error);
        remove_pending(it);
        return;
    }

    // Send LoginResult to client with BaseApp external address + session key
    login::LoginResult result;
    result.status = login::LoginStatus::Success;
    result.session_key = pending.session_key;
    result.baseapp_addr = pending.baseapp_external_addr;
    auto* client_ch = external_network_.find_channel(pending.client_channel_id);
    if (!client_ch)
    {
        ATLAS_LOG_WARNING(
            "LoginApp: client channel disappeared before login completion request_id={}",
            pending.request_id);
        abandon_pending_login(it);
        return;
    }

    if (!client_ch->send_message(result))
    {
        ATLAS_LOG_WARNING("LoginApp: failed to deliver LoginResult request_id={}",
                          pending.request_id);
        abandon_pending_login(it);
        return;
    }

    ++login_success_total_;

    ATLAS_LOG_DEBUG("LoginApp: login complete for '{}' entity={} baseapp={}:{}", pending.username,
                    msg.entity_id, pending.baseapp_external_addr.ip(),
                    pending.baseapp_external_addr.port());
    remove_pending(it);
}

// ============================================================================
// Helpers
// ============================================================================

void LoginApp::cancel_prepare_login(const PendingLogin& pending)
{
    canceled_requests_[pending.request_id] = Clock::now();

    if (pending.baseapp_internal_addr.port() == 0)
    {
        return;
    }

    auto ch_result = network().connect_rudp_nocwnd(pending.baseapp_internal_addr);
    if (!ch_result)
    {
        ATLAS_LOG_WARNING("LoginApp: failed to cancel prepare login request_id={} at {}:{}",
                          pending.request_id, pending.baseapp_internal_addr.ip(),
                          pending.baseapp_internal_addr.port());
        return;
    }

    login::CancelPrepareLogin cancel;
    cancel.request_id = pending.request_id;
    cancel.dbid = pending.dbid;
    (void)(*ch_result)->send_message(cancel);
}

void LoginApp::abandon_pending_login(std::unordered_map<uint32_t, PendingLogin>::iterator it)
{
    PendingLogin pending = std::move(it->second);
    pending_by_username_.erase(pending.username);
    pending_.erase(it);
    cancel_prepare_login(pending);
}

void LoginApp::on_client_disconnect(Channel& ch)
{
    for (auto it = pending_.begin(); it != pending_.end();)
    {
        if (it->second.client_channel_id == ch.channel_id())
        {
            auto current = it++;
            abandon_pending_login(current);
        }
        else
        {
            ++it;
        }
    }
}

void LoginApp::remove_pending(std::unordered_map<uint32_t, PendingLogin>::iterator it)
{
    pending_by_username_.erase(it->second.username);
    pending_.erase(it);
}

void LoginApp::send_login_error(uint64_t client_channel_id, login::LoginStatus status,
                                const std::string& msg)
{
    ++login_fail_total_;
    auto* ch = external_network_.find_channel(client_channel_id);
    if (!ch)
        return;
    login::LoginResult result;
    result.status = status;
    result.error_message = msg;
    (void)ch->send_message(result);
}

auto LoginApp::is_rate_limited(const Address& src) -> bool
{
    const auto now = Clock::now();
    cleanup_stale_rate_entries(now);

    if (rate_limit_per_ip_ <= 0 && rate_limit_global_ <= 0)
    {
        return false;
    }

    if (is_trusted_rate_limit_source(src))
    {
        return false;
    }

    // Global window check
    if (global_window_start_ == TimePoint{})
        global_window_start_ = now;
    if (now - global_window_start_ > rate_limit_window_)
    {
        global_window_start_ = now;
        global_login_count_ = 0;
    }
    if (rate_limit_global_ > 0 && global_login_count_ >= rate_limit_global_)
        return true;

    // Per-IP window check
    if (rate_limit_per_ip_ <= 0)
    {
        return false;
    }

    auto& entry = rate_table_[src.ip()];
    if (entry.window_start == TimePoint{})
        entry.window_start = now;
    if (now - entry.window_start > rate_limit_window_)
    {
        entry.window_start = now;
        entry.count = 0;
    }
    return entry.count >= rate_limit_per_ip_;
}

auto LoginApp::is_trusted_rate_limit_source(const Address& src) const -> bool
{
    return !trusted_rate_limit_sources_.empty() && trusted_rate_limit_sources_.contains(src.ip());
}

void LoginApp::record_login_attempt(const Address& src)
{
    if (is_trusted_rate_limit_source(src))
    {
        return;
    }

    if (rate_limit_global_ > 0)
    {
        ++global_login_count_;
    }
    if (rate_limit_per_ip_ > 0)
    {
        auto& entry = rate_table_[src.ip()];
        if (entry.window_start == TimePoint{})
        {
            entry.window_start = Clock::now();
        }
        ++entry.count;
    }
}

void LoginApp::cleanup_stale_rate_entries(TimePoint now)
{
    if (now - last_rate_cleanup_ < kRateCleanupInterval)
    {
        return;
    }
    last_rate_cleanup_ = now;

    for (auto it = rate_table_.begin(); it != rate_table_.end();)
    {
        if (now - it->second.window_start > rate_limit_window_)
        {
            it = rate_table_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void LoginApp::cleanup_expired_logins()
{
    const auto now = Clock::now();
    for (auto it = pending_.begin(); it != pending_.end();)
    {
        if (now - it->second.created_at > kPendingTimeout)
        {
            ++login_timeout_total_;
            ATLAS_LOG_WARNING("LoginApp: pending login request_id={} timed out",
                              it->second.request_id);
            send_login_error(it->second.client_channel_id, login::LoginStatus::InternalError,
                             "timeout");
            auto current = it++;
            abandon_pending_login(current);
        }
        else
        {
            ++it;
        }
    }
}

void LoginApp::cleanup_canceled_requests(TimePoint now)
{
    std::erase_if(canceled_requests_, [now](const auto& entry)
                  { return now - entry.second > kCanceledRequestRetention; });
}

}  // namespace atlas
