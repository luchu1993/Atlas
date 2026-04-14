#include "dbapp.hpp"

#include "foundation/log.hpp"
#include "loginapp/login_messages.hpp"
#include "network/channel.hpp"
#include "network/machined_types.hpp"
#include "network/reliable_udp.hpp"

#include <format>

namespace atlas
{

// ============================================================================
// run — static entry point
// ============================================================================

auto DBApp::run(int argc, char* argv[]) -> int
{
    EventDispatcher dispatcher;
    NetworkInterface network(dispatcher);
    DBApp app(dispatcher, network);
    return app.run_app(argc, argv);
}

// ============================================================================
// init
// ============================================================================

auto DBApp::init(int argc, char* argv[]) -> bool
{
    if (!ManagerApp::init(argc, argv))
        return false;

    const auto& cfg = config();

    // ---- Load entity definitions from JSON ----------------------------------
    if (cfg.entitydef_path.empty())
    {
        ATLAS_LOG_WARNING("DBApp: no --entitydef-path configured; entity_defs will be empty");
        entity_defs_.emplace();  // empty registry
    }
    else
    {
        auto result = EntityDefRegistry::from_json_file(cfg.entitydef_path);
        if (!result)
        {
            ATLAS_LOG_ERROR("DBApp: failed to load entity_defs from '{}': {}",
                            cfg.entitydef_path.string(), result.error().message());
            return false;
        }
        entity_defs_ = std::move(*result);
        ATLAS_LOG_INFO("DBApp: loaded {} entity types from '{}'", entity_defs_->type_count(),
                       cfg.entitydef_path.string());
    }

    // ---- Create and start database backend ----------------------------------
    auto db_cfg = build_db_config();
    database_ = create_database(db_cfg);
    if (!database_)
    {
        ATLAS_LOG_ERROR("DBApp: unknown database backend '{}'", db_cfg.type);
        return false;
    }

    auto startup_result = database_->startup(db_cfg, *entity_defs_);
    if (!startup_result)
    {
        ATLAS_LOG_ERROR("DBApp: database startup failed: {}", startup_result.error().message());
        return false;
    }
    database_->set_deferred_mode(true);

    // ---- Register message handlers ------------------------------------------
    auto& table = network().interface_table();

    (void)table.register_typed_handler<dbapp::WriteEntity>(
        [this](const Address& src, Channel* ch, const dbapp::WriteEntity& msg)
        { on_write_entity(src, ch, msg); });

    (void)table.register_typed_handler<dbapp::CheckoutEntity>(
        [this](const Address& src, Channel* ch, const dbapp::CheckoutEntity& msg)
        { on_checkout_entity(src, ch, msg); });

    (void)table.register_typed_handler<dbapp::CheckinEntity>(
        [this](const Address& src, Channel* ch, const dbapp::CheckinEntity& msg)
        { on_checkin_entity(src, ch, msg); });

    (void)table.register_typed_handler<dbapp::DeleteEntity>(
        [this](const Address& src, Channel* ch, const dbapp::DeleteEntity& msg)
        { on_delete_entity(src, ch, msg); });

    (void)table.register_typed_handler<dbapp::LookupEntity>(
        [this](const Address& src, Channel* ch, const dbapp::LookupEntity& msg)
        { on_lookup_entity(src, ch, msg); });

    (void)table.register_typed_handler<dbapp::AbortCheckout>(
        [this](const Address& src, Channel* ch, const dbapp::AbortCheckout& msg)
        { on_abort_checkout(src, ch, msg); });

    // ---- Authentication (LoginApp → DBApp) ----------------------------------
    (void)table.register_typed_handler<login::AuthLogin>(
        [this](const Address& src, Channel* ch, const login::AuthLogin& msg)
        { on_auth_login(src, ch, msg); });

    // ---- EntityID allocation handlers ----------------------------------------
    (void)table.register_typed_handler<dbapp::GetEntityIds>(
        [this](const Address& src, Channel* ch, const dbapp::GetEntityIds& msg)
        { on_get_entity_ids(src, ch, msg); });

    (void)table.register_typed_handler<dbapp::PutEntityIds>(
        [this](const Address& src, Channel* ch, const dbapp::PutEntityIds& msg)
        { on_put_entity_ids(src, ch, msg); });

    // ---- Subscribe to BaseApp death notifications ---------------------------
    machined_client().subscribe(machined::ListenerType::Death, ProcessType::BaseApp,
                                nullptr,  // no birth callback needed
                                [this](const machined::DeathNotification& notif)
                                { on_baseapp_death(notif.internal_addr, notif.name); });

    // ---- Configuration ------------------------------------------------------
    auto_create_accounts_ = cfg.auto_create_accounts;
    account_type_id_ = cfg.account_type_id;

    // ---- EntityID allocator — authoritative ID source -----------------------
    id_allocator_ = std::make_unique<EntityIdAllocator>(*database_);
    id_allocator_->startup(
        [](bool ok)
        {
            if (!ok)
                ATLAS_LOG_ERROR("DBApp: EntityIdAllocator startup failed");
            else
                ATLAS_LOG_INFO("DBApp: EntityIdAllocator ready");
        });

    ATLAS_LOG_INFO("DBApp: initialised (backend={}, auto_create={}, account_type={})", db_cfg.type,
                   auto_create_accounts_, account_type_id_);
    return true;
}

// ============================================================================
// fini
// ============================================================================

void DBApp::fini()
{
    if (id_allocator_ && database_)
    {
        id_allocator_->persist(
            [](bool ok)
            {
                if (!ok)
                    ATLAS_LOG_WARNING(
                        "DBApp: final EntityIdAllocator persist "
                        "failed");
            });
        // Flush the deferred persist callback before shutting down the DB
        database_->process_results();
        id_allocator_.reset();
    }
    if (database_)
    {
        database_->shutdown();
        database_.reset();
    }
    ManagerApp::fini();
}

// ============================================================================
// on_tick_complete — pump deferred DB callbacks
// ============================================================================

void DBApp::on_tick_complete()
{
    ManagerApp::on_tick_complete();
    if (database_)
        database_->process_results();
    if (id_allocator_)
        id_allocator_->persist_if_needed([](bool) {});
}

// ============================================================================
// register_watchers
// ============================================================================

void DBApp::register_watchers()
{
    ManagerApp::register_watchers();
    auto& reg = watcher_registry();

    reg.add<std::string>("dbapp/entity_defs_loaded",
                         std::function<std::string()>{[this]()
                                                      {
                                                          return (entity_defs_.has_value() &&
                                                                  entity_defs_->type_count() > 0)
                                                                     ? "yes"
                                                                     : "no";
                                                      }});

    reg.add<std::string>(
        "dbapp/entity_type_count",
        std::function<std::string()>{
            [this]()
            {
                return std::to_string(entity_defs_.has_value() ? entity_defs_->type_count() : 0u);
            }});

    reg.add<std::string>(
        "dbapp/checkouts",
        std::function<std::string()>{[this]() { return std::to_string(checkout_mgr_.size()); }});
    reg.add<std::size_t>(
        "dbapp/pending_checkout_request_count",
        std::function<std::size_t()>([this] { return pending_checkout_requests_.size(); }));
    reg.add<uint64_t>("dbapp/abort_checkout_total",
                      std::function<uint64_t()>([this] { return abort_checkout_total_; }));
    reg.add<uint64_t>(
        "dbapp/abort_checkout_pending_hit_total",
        std::function<uint64_t()>([this] { return abort_checkout_pending_hit_total_; }));
    reg.add<uint64_t>("dbapp/abort_checkout_late_hit_total",
                      std::function<uint64_t()>([this] { return abort_checkout_late_hit_total_; }));

    reg.add<std::string>(
        "dbapp/next_entity_id",
        std::function<std::string()>{
            [this]() { return std::to_string(id_allocator_ ? id_allocator_->next_id() : 0u); }});
}

// ============================================================================
// build_db_config
// ============================================================================

auto DBApp::build_db_config() const -> DatabaseConfig
{
    const auto& cfg = config();
    DatabaseConfig db_cfg;
    db_cfg.type = cfg.db_type;
    db_cfg.xml_dir = cfg.db_xml_dir;
    db_cfg.sqlite_path = cfg.db_sqlite_path;
    db_cfg.sqlite_wal = cfg.db_sqlite_wal;
    db_cfg.sqlite_busy_timeout_ms = cfg.db_sqlite_busy_timeout_ms;
    db_cfg.sqlite_foreign_keys = cfg.db_sqlite_foreign_keys;
    db_cfg.mysql_host = cfg.db_mysql_host;
    db_cfg.mysql_port = cfg.db_mysql_port;
    db_cfg.mysql_user = cfg.db_mysql_user;
    db_cfg.mysql_password = cfg.db_mysql_password;
    db_cfg.mysql_database = cfg.db_mysql_database;
    db_cfg.mysql_pool_size = cfg.db_mysql_pool_size;
    return db_cfg;
}

auto DBApp::resolve_reply_channel(const Address& addr) -> Channel*
{
    if (auto* existing = network().find_channel(addr))
    {
        return existing;
    }

    auto result = network().connect_rudp_nocwnd(addr);
    if (!result)
    {
        ATLAS_LOG_WARNING("DBApp: failed to resolve reply channel {}:{}", addr.ip(), addr.port());
        return nullptr;
    }

    return static_cast<Channel*>(*result);
}

// ============================================================================
// on_write_entity
// ============================================================================

void DBApp::on_write_entity(const Address& src, Channel* ch, const dbapp::WriteEntity& msg)
{
    if (ch == nullptr)
        return;

    auto send_ack = [this, reply_addr = src, request_id = msg.request_id](PutResult result)
    {
        dbapp::WriteEntityAck ack;
        ack.request_id = request_id;
        ack.success = result.success;
        ack.dbid = result.dbid;
        ack.error = std::move(result.error);
        if (auto* reply_ch = this->resolve_reply_channel(reply_addr))
        {
            (void)reply_ch->send_message(ack);
        }
    };

    // Checkin path: LogOff flag means entity is going offline — clear checkout
    if (has_flag(msg.flags, WriteFlags::LogOff))
        checkout_mgr_.checkin(msg.dbid, msg.type_id);

    database_->put_entity(msg.dbid, msg.type_id, msg.flags, msg.blob, msg.identifier,
                          std::move(send_ack));
}

// ============================================================================
// on_checkout_entity
// ============================================================================

void DBApp::on_checkout_entity(const Address& src, Channel* ch, const dbapp::CheckoutEntity& msg)
{
    if (ch == nullptr)
        return;

    ATLAS_LOG_DEBUG("DBApp: checkout request_id={} dbid={} type_id={} from {}:{}", msg.request_id,
                    msg.dbid, msg.type_id, src.ip(), src.port());

    CheckoutInfo owner;
    owner.base_addr = (msg.owner_addr.port() != 0) ? msg.owner_addr : src;
    owner.entity_id = msg.entity_id;

    auto send_ack = [this, reply_addr = src, request_id = msg.request_id](GetResult result)
    {
        dbapp::CheckoutEntityAck ack;
        ack.request_id = request_id;
        ack.dbid = result.data.dbid;

        if (!result.success)
        {
            ack.status = dbapp::CheckoutStatus::NotFound;
            ack.error = std::move(result.error);
        }
        else if (result.checked_out_by.has_value())
        {
            ack.status = dbapp::CheckoutStatus::AlreadyCheckedOut;
            ack.holder_addr = result.checked_out_by->base_addr;
            ack.holder_app_id = result.checked_out_by->app_id;
            ack.holder_entity_id = result.checked_out_by->entity_id;
        }
        else
        {
            ack.status = dbapp::CheckoutStatus::Success;
            ack.blob = std::move(result.data.blob);
        }
        if (auto* reply_ch = this->resolve_reply_channel(reply_addr))
        {
            (void)reply_ch->send_message(ack);
        }
    };

    // Claim the slot in memory first
    auto co_result = checkout_mgr_.try_checkout(
        msg.mode == dbapp::LoadMode::ByDBID ? msg.dbid : kInvalidDBID, msg.type_id, owner);

    if (co_result.status == CheckoutManager::CheckoutStatus::AlreadyCheckedOut ||
        co_result.status == CheckoutManager::CheckoutStatus::PendingCheckout)
    {
        ATLAS_LOG_DEBUG(
            "DBApp: checkout conflict request_id={} dbid={} type_id={} owner={}:{} app_id={} "
            "entity_id={} state={}",
            msg.request_id, msg.dbid, msg.type_id, co_result.current_owner.base_addr.ip(),
            co_result.current_owner.base_addr.port(), co_result.current_owner.app_id,
            co_result.current_owner.entity_id,
            co_result.status == CheckoutManager::CheckoutStatus::AlreadyCheckedOut ? "confirmed"
                                                                                   : "pending");
        dbapp::CheckoutEntityAck ack;
        ack.request_id = msg.request_id;
        ack.status = dbapp::CheckoutStatus::AlreadyCheckedOut;
        ack.dbid = msg.dbid;
        ack.holder_addr = co_result.current_owner.base_addr;
        ack.holder_app_id = co_result.current_owner.app_id;
        ack.holder_entity_id = co_result.current_owner.entity_id;
        (void)ch->send_message(ack);
        return;
    }

    // Success path: delegate to DB backend
    // After DB confirms, promote checkout to Confirmed; on failure, roll back.
    auto dbid = msg.dbid;
    auto type_id = msg.type_id;
    pending_checkout_requests_[msg.request_id] =
        PendingCheckoutRequest{dbid, type_id, src, false, kInvalidDBID};

    auto on_db_done = [this, request_id = msg.request_id, dbid, type_id,
                       send_ack = std::move(send_ack)](GetResult result) mutable
    {
        bool canceled = false;
        DatabaseID cleanup_dbid = dbid;
        DatabaseID cleared_dbid = kInvalidDBID;
        if (auto pending_it = pending_checkout_requests_.find(request_id);
            pending_it != pending_checkout_requests_.end())
        {
            canceled = pending_it->second.canceled;
            if (pending_it->second.dbid != kInvalidDBID)
            {
                cleanup_dbid = pending_it->second.dbid;
            }
            cleared_dbid = pending_it->second.cleared_dbid;
            pending_checkout_requests_.erase(pending_it);
        }

        if (result.success && result.data.dbid != kInvalidDBID)
        {
            cleanup_dbid = result.data.dbid;
        }

        if (canceled)
        {
            if (cleanup_dbid != cleared_dbid)
            {
                checkout_mgr_.release_checkout(cleanup_dbid, type_id);
                if (cleanup_dbid != kInvalidDBID)
                {
                    database_->mark_checkout_cleared(cleanup_dbid, type_id);
                }
            }
            ATLAS_LOG_DEBUG("DBApp: checkout request_id={} canceled before reply", request_id);
            return;
        }

        if (!result.success || result.checked_out_by.has_value())
        {
            checkout_mgr_.release_checkout(cleanup_dbid, type_id);
        }
        else
        {
            checkout_mgr_.confirm_checkout(result.data.dbid, type_id);
        }
        send_ack(std::move(result));
    };

    if (msg.mode == dbapp::LoadMode::ByName)
    {
        database_->checkout_entity_by_name(msg.type_id, msg.identifier, owner,
                                           std::move(on_db_done));
    }
    else
    {
        database_->checkout_entity(msg.dbid, msg.type_id, owner, std::move(on_db_done));
    }
}

void DBApp::on_abort_checkout(const Address& src, Channel* ch, const dbapp::AbortCheckout& msg)
{
    if (ch == nullptr)
        return;

    ++abort_checkout_total_;
    auto it = pending_checkout_requests_.find(msg.request_id);
    if (it != pending_checkout_requests_.end())
    {
        ++abort_checkout_pending_hit_total_;
        it->second.canceled = true;
        if (it->second.dbid != kInvalidDBID && it->second.cleared_dbid != it->second.dbid)
        {
            checkout_mgr_.release_checkout(it->second.dbid, it->second.type_id);
            database_->mark_checkout_cleared(it->second.dbid, it->second.type_id);
            it->second.cleared_dbid = it->second.dbid;
        }
    }
    else
    {
        ++abort_checkout_late_hit_total_;
        checkout_mgr_.checkin(msg.dbid, msg.type_id);
        if (msg.dbid != kInvalidDBID)
        {
            database_->mark_checkout_cleared(msg.dbid, msg.type_id);
        }
    }

    dbapp::AbortCheckoutAck ack;
    ack.request_id = msg.request_id;
    ack.success = true;
    if (auto* reply_ch = this->resolve_reply_channel(src))
    {
        (void)reply_ch->send_message(ack);
    }
}

// ============================================================================
// on_checkin_entity
// ============================================================================

void DBApp::on_checkin_entity(const Address& /*src*/, Channel* /*ch*/,
                              const dbapp::CheckinEntity& msg)
{
    ATLAS_LOG_DEBUG("DBApp: checkin dbid={} type_id={}", msg.dbid, msg.type_id);
    checkout_mgr_.checkin(msg.dbid, msg.type_id);
    database_->mark_checkout_cleared(msg.dbid, msg.type_id);
}

// ============================================================================
// on_delete_entity
// ============================================================================

void DBApp::on_delete_entity(const Address& src, Channel* ch, const dbapp::DeleteEntity& msg)
{
    if (ch == nullptr)
        return;

    database_->del_entity(msg.dbid, msg.type_id,
                          [this, reply_addr = src, request_id = msg.request_id](DelResult result)
                          {
                              dbapp::DeleteEntityAck ack;
                              ack.request_id = request_id;
                              ack.success = result.success;
                              ack.error = std::move(result.error);
                              if (auto* reply_ch = this->resolve_reply_channel(reply_addr))
                              {
                                  (void)reply_ch->send_message(ack);
                              }
                          });
}

// ============================================================================
// on_lookup_entity
// ============================================================================

void DBApp::on_lookup_entity(const Address& src, Channel* ch, const dbapp::LookupEntity& msg)
{
    if (ch == nullptr)
        return;

    database_->lookup_by_name(
        msg.type_id, msg.identifier,
        [this, reply_addr = src, request_id = msg.request_id](LookupResult result)
        {
            dbapp::LookupEntityAck ack;
            ack.request_id = request_id;
            ack.found = result.found;
            ack.dbid = result.dbid;
            if (auto* reply_ch = this->resolve_reply_channel(reply_addr))
            {
                (void)reply_ch->send_message(ack);
            }
        });
}

// ============================================================================
// on_get_entity_ids — allocate EntityIDs for a BaseApp
// ============================================================================

void DBApp::on_get_entity_ids(const Address& src, Channel* ch, const dbapp::GetEntityIds& msg)
{
    if (ch == nullptr)
        return;

    if (!id_allocator_)
    {
        ATLAS_LOG_ERROR("DBApp: GetEntityIds but allocator not ready");
        return;
    }

    auto [start, end] = id_allocator_->allocate(msg.count);
    ATLAS_LOG_DEBUG("DBApp: allocated entity IDs [{}, {}] (count={}) for {}:{}", start, end,
                    msg.count, src.ip(), src.port());

    dbapp::GetEntityIdsAck ack;
    ack.start = start;
    ack.end = end;
    ack.count = msg.count;
    if (auto* reply_ch = resolve_reply_channel(src))
    {
        (void)reply_ch->send_message(ack);
    }
}

// ============================================================================
// on_put_entity_ids — return unused EntityIDs (currently acknowledged only)
// ============================================================================

void DBApp::on_put_entity_ids(const Address& src, Channel* ch, const dbapp::PutEntityIds& msg)
{
    if (ch == nullptr)
        return;

    ATLAS_LOG_DEBUG("DBApp: PutEntityIds [{}, {}] from {}:{} (not recycled)", msg.start, msg.end,
                    src.ip(), src.port());

    dbapp::PutEntityIdsAck ack;
    ack.success = true;
    if (auto* reply_ch = resolve_reply_channel(src))
    {
        (void)reply_ch->send_message(ack);
    }
}

// ============================================================================
// on_baseapp_death
// ============================================================================

void DBApp::on_baseapp_death(const Address& internal_addr, std::string_view name)
{
    int cleared_memory = checkout_mgr_.clear_all_for(internal_addr);
    ATLAS_LOG_WARNING("DBApp: BaseApp '{}' died — cleared {} memory checkouts", std::string(name),
                      cleared_memory);

    database_->clear_checkouts_for_address(
        internal_addr,
        [name_str = std::string(name)](int cleared_db)
        {
            ATLAS_LOG_WARNING("DBApp: cleared {} DB checkouts for dead BaseApp '{}'", cleared_db,
                              name_str);
        });
}

// ============================================================================
// on_auth_login — handle LoginApp authentication request
// ============================================================================

void DBApp::on_auth_login(const Address& src, Channel* ch, const login::AuthLogin& msg)
{
    ATLAS_LOG_DEBUG("DBApp: auth login request_id={} user='{}' from {}:{}", msg.request_id,
                    msg.username, src.ip(), src.port());
    if (!database_)
    {
        login::AuthLoginResult reply;
        reply.request_id = msg.request_id;
        reply.success = false;
        reply.status = login::LoginStatus::InternalError;
        if (auto* reply_ch = this->resolve_reply_channel(src))
        {
            (void)reply_ch->send_message(reply);
        }
        return;
    }

    // Look up the account by username (identifier column)
    database_->lookup_by_name(
        account_type_id_, msg.username,
        [this, request_id = msg.request_id, password_hash = msg.password_hash,
         auto_create = msg.auto_create, username = msg.username,
         reply_addr = src](LookupResult result)
        {
            login::AuthLoginResult reply;
            reply.request_id = request_id;

            // A backend error (e.g. transient SQLite failure) must not be
            // mistaken for "account does not exist" — surface it immediately.
            if (!result.error.empty())
            {
                ATLAS_LOG_ERROR("DBApp: auth lookup failed for '{}': {}", username, result.error);
                reply.success = false;
                reply.status = login::LoginStatus::InternalError;
                if (auto* reply_ch = this->resolve_reply_channel(reply_addr))
                {
                    (void)reply_ch->send_message(reply);
                }
                return;
            }

            if (!result.found)
            {
                if (auto_create && auto_create_accounts_)
                {
                    // Create new account entity with the credential hash stored in the DB row.
                    EntityData data;
                    data.type_id = account_type_id_;
                    data.identifier = username;
                    // Empty blob for now — the entity will be populated by C# on first login.
                    data.blob = {};
                    database_->put_entity_with_password(
                        kInvalidDBID, account_type_id_, WriteFlags::CreateNew, data.blob,
                        data.identifier, password_hash,
                        [this, reply_addr, request_id, type_id = account_type_id_](PutResult put)
                        {
                            login::AuthLoginResult r;
                            r.request_id = request_id;
                            if (put.success)
                            {
                                r.success = true;
                                r.status = login::LoginStatus::Success;
                                r.dbid = put.dbid;
                                r.type_id = type_id;
                            }
                            else
                            {
                                r.success = false;
                                r.status = login::LoginStatus::InternalError;
                            }
                            if (auto* reply_ch = this->resolve_reply_channel(reply_addr))
                            {
                                (void)reply_ch->send_message(r);
                            }
                        });
                    return;
                }
                reply.success = false;
                reply.status = login::LoginStatus::InvalidCredentials;
            }
            else
            {
                // Simple hash comparison: compare provided hash to stored one
                bool pw_match = true;
                if (!password_hash.empty() && !result.password_hash.empty())
                    pw_match = (result.password_hash == password_hash);

                if (!pw_match)
                {
                    reply.success = false;
                    reply.status = login::LoginStatus::InvalidCredentials;
                }
                else
                {
                    reply.success = true;
                    reply.status = login::LoginStatus::Success;
                    reply.dbid = result.dbid;
                    reply.type_id = account_type_id_;
                }
            }
            if (auto* reply_ch = this->resolve_reply_channel(reply_addr))
            {
                (void)reply_ch->send_message(reply);
            }
        });
}

}  // namespace atlas
