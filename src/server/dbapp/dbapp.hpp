#pragma once

#include "checkout_manager.hpp"
#include "db/database_factory.hpp"
#include "db/idatabase.hpp"
#include "dbapp_messages.hpp"
#include "entitydef/entity_def_registry.hpp"
#include "server/manager_app.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace atlas
{

namespace login
{
struct AuthLogin;
struct AuthLoginResult;
}  // namespace login

class DBApp : public ManagerApp
{
public:
    using ManagerApp::ManagerApp;

    static auto run(int argc, char* argv[]) -> int;

protected:
    auto init(int argc, char* argv[]) -> bool override;
    void fini() override;
    void on_tick_complete() override;
    void register_watchers() override;

private:
    friend class DBAppRollbackTest;

    // ---- Message handlers ---------------------------------------------------
    void on_write_entity(const Address& src, Channel* ch, const dbapp::WriteEntity& msg);
    void on_checkout_entity(const Address& src, Channel* ch, const dbapp::CheckoutEntity& msg);
    void on_checkin_entity(const Address& src, Channel* ch, const dbapp::CheckinEntity& msg);
    void on_delete_entity(const Address& src, Channel* ch, const dbapp::DeleteEntity& msg);
    void on_lookup_entity(const Address& src, Channel* ch, const dbapp::LookupEntity& msg);
    void on_abort_checkout(const Address& src, Channel* ch, const dbapp::AbortCheckout& msg);

    // ---- Authentication (LoginApp → DBApp) ----------------------------------
    void on_auth_login(const Address& src, Channel* ch, const login::AuthLogin& msg);

    // ---- BaseApp death notification -----------------------------------------
    void on_baseapp_death(const Address& internal_addr, std::string_view name);

    // ---- Helpers ------------------------------------------------------------
    [[nodiscard]] auto build_db_config() const -> DatabaseConfig;
    auto resolve_reply_channel(const Address& addr) -> Channel*;

    // ---- State --------------------------------------------------------------
    std::unique_ptr<IDatabase> database_;
    CheckoutManager checkout_mgr_;
    struct PendingCheckoutRequest
    {
        DatabaseID dbid{kInvalidDBID};
        uint16_t type_id{0};
        Address reply_addr;
        bool canceled{false};
        DatabaseID cleared_dbid{kInvalidDBID};
    };
    std::unordered_map<uint32_t, PendingCheckoutRequest> pending_checkout_requests_;
    std::optional<EntityDefRegistry> entity_defs_;  // nullopt until loaded
    bool auto_create_accounts_{false};
    uint16_t account_type_id_{0};
    uint64_t abort_checkout_total_{0};
    uint64_t abort_checkout_pending_hit_total_{0};
    uint64_t abort_checkout_late_hit_total_{0};
};

}  // namespace atlas
