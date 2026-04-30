#include "dbapp.h"

#include <format>

#include "foundation/log.h"
#include "loginapp/login_messages.h"
#include "network/channel.h"
#include "network/machined_types.h"
#include "network/reliable_udp.h"

namespace atlas {

auto DBApp::Run(int argc, char* argv[]) -> int {
  EventDispatcher dispatcher;
  NetworkInterface network(dispatcher);
  DBApp app(dispatcher, network);
  return app.RunApp(argc, argv);
}

auto DBApp::Init(int argc, char* argv[]) -> bool {
  if (!ManagerApp::Init(argc, argv)) return false;

  const auto& cfg = Config();

  // ATDF binary container produced by Atlas.Tools.DefDump from a built
  // C# server assembly. DBApp doesn't host CoreCLR, so it can't run the
  // codegen-emitted [ModuleInitializer] PInvoke registrations - the
  // offline DefDump mirrors that path and writes a wire-compatible blob
  // for RegisterFromBinaryFile to ingest.
  if (cfg.entitydef_bin_path.empty()) {
    ATLAS_LOG_WARNING("DBApp: no --entitydef-bin-path configured; entity_defs will be empty");
    entity_defs_.emplace();
  } else {
    entity_defs_.emplace();
    auto result = entity_defs_->RegisterFromBinaryFile(cfg.entitydef_bin_path);
    if (!result) {
      ATLAS_LOG_ERROR("DBApp: failed to load entity_defs from '{}': {}",
                      cfg.entitydef_bin_path.string(), result.Error().Message());
      return false;
    }
    ATLAS_LOG_INFO("DBApp: loaded entity_defs from '{}' (structs={}, components={}, types={})",
                   cfg.entitydef_bin_path.string(), result->structs, result->components,
                   result->types);
  }

  auto db_cfg = BuildDbConfig();
  database_ = CreateDatabase(db_cfg);
  if (!database_) {
    ATLAS_LOG_ERROR("DBApp: unknown database backend '{}'", db_cfg.type);
    return false;
  }

  auto startup_result = database_->Startup(db_cfg, *entity_defs_);
  if (!startup_result) {
    ATLAS_LOG_ERROR("DBApp: database startup failed: {}", startup_result.Error().Message());
    return false;
  }
  database_->SetDeferredMode(true);
  database_->BeginBatch();

  auto& table = Network().InterfaceTable();

  (void)table.RegisterTypedHandler<dbapp::WriteEntity>(
      [this](const Address& src, Channel* ch, const dbapp::WriteEntity& msg) {
        OnWriteEntity(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<dbapp::CheckoutEntity>(
      [this](const Address& src, Channel* ch, const dbapp::CheckoutEntity& msg) {
        OnCheckoutEntity(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<dbapp::CheckinEntity>(
      [this](const Address& src, Channel* ch, const dbapp::CheckinEntity& msg) {
        OnCheckinEntity(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<dbapp::DeleteEntity>(
      [this](const Address& src, Channel* ch, const dbapp::DeleteEntity& msg) {
        OnDeleteEntity(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<dbapp::LookupEntity>(
      [this](const Address& src, Channel* ch, const dbapp::LookupEntity& msg) {
        OnLookupEntity(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<dbapp::AbortCheckout>(
      [this](const Address& src, Channel* ch, const dbapp::AbortCheckout& msg) {
        OnAbortCheckout(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<login::AuthLogin>(
      [this](const Address& src, Channel* ch, const login::AuthLogin& msg) {
        OnAuthLogin(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<dbapp::GetEntityIds>(
      [this](const Address& src, Channel* ch, const dbapp::GetEntityIds& msg) {
        OnGetEntityIds(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<dbapp::PutEntityIds>(
      [this](const Address& src, Channel* ch, const dbapp::PutEntityIds& msg) {
        OnPutEntityIds(src, ch, msg);
      });

  GetMachinedClient().Subscribe(machined::ListenerType::kDeath, ProcessType::kBaseApp,
                                nullptr,  // no birth callback needed
                                [this](const machined::DeathNotification& notif) {
                                  OnBaseappDeath(notif.internal_addr, notif.name);
                                });

  auto_create_accounts_ = cfg.auto_create_accounts;
  account_type_id_ = cfg.account_type_id;

  id_allocator_ = std::make_unique<EntityIdAllocator>(*database_);
  id_allocator_->Startup([](bool ok) {
    if (!ok)
      ATLAS_LOG_ERROR("DBApp: EntityIdAllocator startup failed");
    else
      ATLAS_LOG_INFO("DBApp: EntityIdAllocator ready");
  });

  ATLAS_LOG_INFO("DBApp: initialised (backend={}, auto_create={}, account_type={})", db_cfg.type,
                 auto_create_accounts_, account_type_id_);
  return true;
}

void DBApp::Fini() {
  if (id_allocator_ && database_) {
    id_allocator_->Persist([](bool ok) {
      if (!ok)
        ATLAS_LOG_WARNING(
            "DBApp: final EntityIdAllocator persist "
            "failed");
    });
    // Flush the deferred persist callback before shutting down the DB
    database_->ProcessResults();
    id_allocator_.reset();
  }
  if (database_) {
    database_->EndBatch();
    database_->Shutdown();
    database_.reset();
  }
  ManagerApp::Fini();
}

void DBApp::OnTickComplete() {
  ManagerApp::OnTickComplete();
  if (database_) {
    database_->EndBatch();        // Commit batched writes from this tick
    database_->ProcessResults();  // Pump deferred callbacks
    database_->BeginBatch();      // Open batch for next tick
  }
  if (id_allocator_) id_allocator_->PersistIfNeeded([](bool) {});
}

void DBApp::RegisterWatchers() {
  ManagerApp::RegisterWatchers();
  auto& reg = GetWatcherRegistry();

  reg.Add<std::string>("dbapp/entity_defs_loaded", std::function<std::string()>{[this]() {
                         return (entity_defs_.has_value() && entity_defs_->TypeCount() > 0) ? "yes"
                                                                                            : "no";
                       }});

  reg.Add<std::string>(
      "dbapp/entity_type_count", std::function<std::string()>{[this]() {
        return std::to_string(entity_defs_.has_value() ? entity_defs_->TypeCount() : 0u);
      }});

  reg.Add<std::string>("dbapp/checkouts", std::function<std::string()>{[this]() {
                         return std::to_string(checkout_mgr_.size());
                       }});
  reg.Add<std::size_t>("dbapp/pending_checkout_request_count", std::function<std::size_t()>([this] {
                         return pending_checkout_requests_.size();
                       }));
  reg.Add<uint64_t>("dbapp/abort_checkout_total",
                    std::function<uint64_t()>([this] { return abort_checkout_total_; }));
  reg.Add<uint64_t>("dbapp/abort_checkout_pending_hit_total", std::function<uint64_t()>([this] {
                      return abort_checkout_pending_hit_total_;
                    }));
  reg.Add<uint64_t>("dbapp/abort_checkout_late_hit_total",
                    std::function<uint64_t()>([this] { return abort_checkout_late_hit_total_; }));

  reg.Add<std::string>("dbapp/next_entity_id", std::function<std::string()>{[this]() {
                         return std::to_string(id_allocator_ ? id_allocator_->next_id() : 0u);
                       }});
}

// build_db_config

auto DBApp::BuildDbConfig() const -> DatabaseConfig {
  const auto& cfg = Config();
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

auto DBApp::ResolveReplyChannel(const Address& addr) -> Channel* {
  if (auto* existing = Network().FindChannel(addr)) {
    return existing;
  }

  auto result = Network().ConnectRudpNocwnd(addr);
  if (!result) {
    ATLAS_LOG_WARNING("DBApp: failed to resolve reply channel {}:{}", addr.Ip(), addr.Port());
    return nullptr;
  }

  return static_cast<Channel*>(*result);
}

// on_write_entity

void DBApp::OnWriteEntity(const Address& src, Channel* ch, const dbapp::WriteEntity& msg) {
  if (ch == nullptr) return;

  auto send_ack = [this, reply_addr = src, request_id = msg.request_id](PutResult result) {
    dbapp::WriteEntityAck ack;
    ack.request_id = request_id;
    ack.success = result.success;
    ack.dbid = result.dbid;
    ack.error = std::move(result.error);
    if (auto* reply_ch = this->ResolveReplyChannel(reply_addr)) {
      if (auto r = reply_ch->SendMessage(ack); !r) {
        // BaseApp's pending-write entry never resolves - Proxy keeps
        // thinking the save is in flight; for kLogOff writes the entity
        // may already be freed before learning the DB rejected it.
        ATLAS_LOG_ERROR(
            "DBApp: WriteEntityAck dropped, dbid={} request_id={} success={} to {}: {} "
            "— durability boundary desync",
            ack.dbid, ack.request_id, ack.success, reply_addr.ToString(), r.Error().Message());
      }
    }
  };

  // Checkin path: LogOff flag means entity is going offline - clear checkout
  if (HasFlag(msg.flags, WriteFlags::kLogOff)) checkout_mgr_.Checkin(msg.dbid, msg.type_id);

  database_->PutEntity(msg.dbid, msg.type_id, msg.flags, msg.blob, msg.identifier,
                       std::move(send_ack));
}

// on_checkout_entity

void DBApp::OnCheckoutEntity(const Address& src, Channel* ch, const dbapp::CheckoutEntity& msg) {
  if (ch == nullptr) return;

  ATLAS_LOG_DEBUG("DBApp: checkout request_id={} dbid={} type_id={} from {}:{}", msg.request_id,
                  msg.dbid, msg.type_id, src.Ip(), src.Port());

  CheckoutInfo owner;
  owner.base_addr = (msg.owner_addr.Port() != 0) ? msg.owner_addr : src;
  owner.entity_id = msg.entity_id;

  auto send_ack = [this, reply_addr = src, request_id = msg.request_id](GetResult result) {
    dbapp::CheckoutEntityAck ack;
    ack.request_id = request_id;
    ack.dbid = result.data.dbid;

    if (!result.success) {
      ack.status = dbapp::CheckoutStatus::kNotFound;
      ack.error = std::move(result.error);
    } else if (result.checked_out_by.has_value()) {
      ack.status = dbapp::CheckoutStatus::kAlreadyCheckedOut;
      ack.holder_addr = result.checked_out_by->base_addr;
      ack.holder_app_id = result.checked_out_by->app_id;
      ack.holder_entity_id = result.checked_out_by->entity_id;
    } else {
      ack.status = dbapp::CheckoutStatus::kSuccess;
      ack.blob = std::move(result.data.blob);
    }
    if (auto* reply_ch = this->ResolveReplyChannel(reply_addr)) {
      if (auto r = reply_ch->SendMessage(ack); !r) {
        // checkout_mgr_ has already committed; if BaseApp never sees the
        // ack, login times out but the DBID stays checked out until
        // BaseApp death detection clears it - login retries from any
        // BaseApp will fail in the meantime.
        ATLAS_LOG_ERROR(
            "DBApp: CheckoutEntityAck dropped, dbid={} request_id={} status={} to {}: {} "
            "— DBID stays checked out, login wedge",
            ack.dbid, ack.request_id, static_cast<int>(ack.status), reply_addr.ToString(),
            r.Error().Message());
      }
    }
  };

  // Claim the slot in memory first
  auto co_result = checkout_mgr_.TryCheckout(
      msg.mode == dbapp::LoadMode::kByDbid ? msg.dbid : kInvalidDBID, msg.type_id, owner);

  if (co_result.status == CheckoutManager::CheckoutStatus::kAlreadyCheckedOut ||
      co_result.status == CheckoutManager::CheckoutStatus::kPendingCheckout) {
    ATLAS_LOG_DEBUG(
        "DBApp: checkout conflict request_id={} dbid={} type_id={} owner={}:{} app_id={} "
        "entity_id={} state={}",
        msg.request_id, msg.dbid, msg.type_id, co_result.current_owner.base_addr.Ip(),
        co_result.current_owner.base_addr.Port(), co_result.current_owner.app_id,
        co_result.current_owner.entity_id,
        co_result.status == CheckoutManager::CheckoutStatus::kAlreadyCheckedOut ? "confirmed"
                                                                                : "pending");
    dbapp::CheckoutEntityAck ack;
    ack.request_id = msg.request_id;
    ack.status = dbapp::CheckoutStatus::kAlreadyCheckedOut;
    ack.dbid = msg.dbid;
    ack.holder_addr = co_result.current_owner.base_addr;
    ack.holder_app_id = co_result.current_owner.app_id;
    ack.holder_entity_id = co_result.current_owner.entity_id;
    (void)ch->SendMessage(ack);
    return;
  }

  // Success path: delegate to DB backend
  // After DB confirms, promote checkout to Confirmed; on failure, roll back.
  auto dbid = msg.dbid;
  auto type_id = msg.type_id;
  pending_checkout_requests_[msg.request_id] =
      PendingCheckoutRequest{dbid, type_id, src, false, kInvalidDBID};

  auto on_db_done = [this, request_id = msg.request_id, dbid, type_id,
                     send_ack = std::move(send_ack)](GetResult result) mutable {
    bool canceled = false;
    DatabaseID cleanup_dbid = dbid;
    DatabaseID cleared_dbid = kInvalidDBID;
    if (auto pending_it = pending_checkout_requests_.find(request_id);
        pending_it != pending_checkout_requests_.end()) {
      canceled = pending_it->second.canceled;
      if (pending_it->second.dbid != kInvalidDBID) {
        cleanup_dbid = pending_it->second.dbid;
      }
      cleared_dbid = pending_it->second.cleared_dbid;
      pending_checkout_requests_.erase(pending_it);
    }

    if (result.success && result.data.dbid != kInvalidDBID) {
      cleanup_dbid = result.data.dbid;
    }

    if (canceled) {
      if (cleanup_dbid != cleared_dbid) {
        checkout_mgr_.ReleaseCheckout(cleanup_dbid, type_id);
        if (cleanup_dbid != kInvalidDBID) {
          database_->MarkCheckoutCleared(cleanup_dbid, type_id);
        }
      }
      ATLAS_LOG_DEBUG("DBApp: checkout request_id={} canceled before reply", request_id);
      return;
    }

    if (!result.success || result.checked_out_by.has_value()) {
      checkout_mgr_.ReleaseCheckout(cleanup_dbid, type_id);
    } else {
      checkout_mgr_.ConfirmCheckout(result.data.dbid, type_id);
    }
    send_ack(std::move(result));
  };

  if (msg.mode == dbapp::LoadMode::kByName) {
    database_->CheckoutEntityByName(msg.type_id, msg.identifier, owner, std::move(on_db_done));
  } else {
    database_->CheckoutEntity(msg.dbid, msg.type_id, owner, std::move(on_db_done));
  }
}

void DBApp::OnAbortCheckout(const Address& src, Channel* ch, const dbapp::AbortCheckout& msg) {
  if (ch == nullptr) return;

  ++abort_checkout_total_;
  auto it = pending_checkout_requests_.find(msg.request_id);
  if (it != pending_checkout_requests_.end()) {
    ++abort_checkout_pending_hit_total_;
    it->second.canceled = true;
    if (it->second.dbid != kInvalidDBID && it->second.cleared_dbid != it->second.dbid) {
      checkout_mgr_.ReleaseCheckout(it->second.dbid, it->second.type_id);
      database_->MarkCheckoutCleared(it->second.dbid, it->second.type_id);
      it->second.cleared_dbid = it->second.dbid;
    }
  } else {
    ++abort_checkout_late_hit_total_;
    checkout_mgr_.Checkin(msg.dbid, msg.type_id);
    if (msg.dbid != kInvalidDBID) {
      database_->MarkCheckoutCleared(msg.dbid, msg.type_id);
    }
  }

  dbapp::AbortCheckoutAck ack;
  ack.request_id = msg.request_id;
  ack.success = true;
  if (auto* reply_ch = this->ResolveReplyChannel(src)) {
    (void)reply_ch->SendMessage(ack);
  }
}

// on_checkin_entity

void DBApp::OnCheckinEntity(const Address& /*src*/, Channel* /*ch*/,
                            const dbapp::CheckinEntity& msg) {
  ATLAS_LOG_DEBUG("DBApp: checkin dbid={} type_id={}", msg.dbid, msg.type_id);
  checkout_mgr_.Checkin(msg.dbid, msg.type_id);
  database_->MarkCheckoutCleared(msg.dbid, msg.type_id);
}

// on_delete_entity

void DBApp::OnDeleteEntity(const Address& src, Channel* ch, const dbapp::DeleteEntity& msg) {
  if (ch == nullptr) return;

  database_->DelEntity(msg.dbid, msg.type_id,
                       [this, reply_addr = src, request_id = msg.request_id](DelResult result) {
                         dbapp::DeleteEntityAck ack;
                         ack.request_id = request_id;
                         ack.success = result.success;
                         ack.error = std::move(result.error);
                         if (auto* reply_ch = this->ResolveReplyChannel(reply_addr)) {
                           (void)reply_ch->SendMessage(ack);
                         }
                       });
}

// on_lookup_entity

void DBApp::OnLookupEntity(const Address& src, Channel* ch, const dbapp::LookupEntity& msg) {
  if (ch == nullptr) return;

  database_->LookupByName(
      msg.type_id, msg.identifier,
      [this, reply_addr = src, request_id = msg.request_id](LookupResult result) {
        dbapp::LookupEntityAck ack;
        ack.request_id = request_id;
        ack.found = result.found;
        ack.dbid = result.dbid;
        if (auto* reply_ch = this->ResolveReplyChannel(reply_addr)) {
          (void)reply_ch->SendMessage(ack);
        }
      });
}

void DBApp::OnGetEntityIds(const Address& src, Channel* ch, const dbapp::GetEntityIds& msg) {
  if (ch == nullptr) return;

  if (!id_allocator_) {
    ATLAS_LOG_ERROR("DBApp: GetEntityIds but allocator not ready");
    return;
  }

  auto [start, end] = id_allocator_->Allocate(msg.count);
  ATLAS_LOG_DEBUG("DBApp: allocated entity IDs [{}, {}] (count={}) for {}:{}", start, end,
                  msg.count, src.Ip(), src.Port());

  dbapp::GetEntityIdsAck ack;
  ack.start = start;
  ack.end = end;
  ack.count = msg.count;
  if (auto* reply_ch = ResolveReplyChannel(src)) {
    (void)reply_ch->SendMessage(ack);
  }
}

void DBApp::OnPutEntityIds(const Address& src, Channel* ch, const dbapp::PutEntityIds& msg) {
  if (ch == nullptr) return;

  ATLAS_LOG_DEBUG("DBApp: PutEntityIds [{}, {}] from {}:{} (not recycled)", msg.start, msg.end,
                  src.Ip(), src.Port());

  dbapp::PutEntityIdsAck ack;
  ack.success = true;
  if (auto* reply_ch = ResolveReplyChannel(src)) {
    (void)reply_ch->SendMessage(ack);
  }
}

// on_baseapp_death

void DBApp::OnBaseappDeath(const Address& internal_addr, std::string_view name) {
  int cleared_memory = checkout_mgr_.ClearAllFor(internal_addr);
  ATLAS_LOG_WARNING("DBApp: BaseApp '{}' died — cleared {} memory checkouts", std::string(name),
                    cleared_memory);

  database_->ClearCheckoutsForAddress(internal_addr, [name_str =
                                                          std::string(name)](int cleared_db) {
    ATLAS_LOG_WARNING("DBApp: cleared {} DB checkouts for dead BaseApp '{}'", cleared_db, name_str);
  });
}

void DBApp::OnAuthLogin(const Address& src, Channel* ch, const login::AuthLogin& msg) {
  ATLAS_LOG_DEBUG("DBApp: auth login request_id={} user='{}' from {}:{}", msg.request_id,
                  msg.username, src.Ip(), src.Port());
  if (!database_) {
    login::AuthLoginResult reply;
    reply.request_id = msg.request_id;
    reply.success = false;
    reply.status = login::LoginStatus::kInternalError;
    if (auto* reply_ch = this->ResolveReplyChannel(src)) {
      (void)reply_ch->SendMessage(reply);
    }
    return;
  }

  // Look up the account by username (identifier column)
  database_->LookupByName(
      account_type_id_, msg.username,
      [this, request_id = msg.request_id, password_hash = msg.password_hash,
       auto_create = msg.auto_create, username = msg.username,
       reply_addr = src](LookupResult result) {
        login::AuthLoginResult reply;
        reply.request_id = request_id;

        // A backend error (e.g. transient SQLite failure) must not be
        // mistaken for "account does not exist" - surface it immediately.
        if (!result.error.empty()) {
          ATLAS_LOG_ERROR("DBApp: auth lookup failed for '{}': {}", username, result.error);
          reply.success = false;
          reply.status = login::LoginStatus::kInternalError;
          if (auto* reply_ch = this->ResolveReplyChannel(reply_addr)) {
            (void)reply_ch->SendMessage(reply);
          }
          return;
        }

        if (!result.found) {
          if (auto_create && auto_create_accounts_) {
            // Create new account entity with the credential hash stored in the DB row.
            EntityData data;
            data.type_id = account_type_id_;
            data.identifier = username;
            // Empty blob for now - the entity will be populated by C# on first login.
            data.blob = {};
            database_->PutEntityWithPassword(
                kInvalidDBID, account_type_id_, WriteFlags::kCreateNew, data.blob, data.identifier,
                password_hash,
                [this, reply_addr, request_id, type_id = account_type_id_](PutResult put) {
                  login::AuthLoginResult r;
                  r.request_id = request_id;
                  if (put.success) {
                    r.success = true;
                    r.status = login::LoginStatus::kSuccess;
                    r.dbid = put.dbid;
                    r.type_id = type_id;
                  } else {
                    r.success = false;
                    r.status = login::LoginStatus::kInternalError;
                  }
                  if (auto* reply_ch = this->ResolveReplyChannel(reply_addr)) {
                    (void)reply_ch->SendMessage(r);
                  }
                });
            return;
          }
          reply.success = false;
          reply.status = login::LoginStatus::kInvalidCredentials;
        } else {
          // Simple hash comparison: compare provided hash to stored one
          bool pw_match = true;
          if (!password_hash.empty() && !result.password_hash.empty())
            pw_match = (result.password_hash == password_hash);

          if (!pw_match) {
            reply.success = false;
            reply.status = login::LoginStatus::kInvalidCredentials;
          } else {
            reply.success = true;
            reply.status = login::LoginStatus::kSuccess;
            reply.dbid = result.dbid;
            reply.type_id = account_type_id_;
          }
        }
        if (auto* reply_ch = this->ResolveReplyChannel(reply_addr)) {
          (void)reply_ch->SendMessage(reply);
        }
      });
}

}  // namespace atlas
