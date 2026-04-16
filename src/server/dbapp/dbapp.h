#ifndef ATLAS_SERVER_DBAPP_DBAPP_H_
#define ATLAS_SERVER_DBAPP_DBAPP_H_

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "checkout_manager.h"
#include "db/database_factory.h"
#include "db/idatabase.h"
#include "dbapp_messages.h"
#include "entity_id_allocator.h"
#include "entitydef/entity_def_registry.h"
#include "server/manager_app.h"

namespace atlas {

namespace login {
struct AuthLogin;
struct AuthLoginResult;
}  // namespace login

class DBApp : public ManagerApp {
 public:
  using ManagerApp::ManagerApp;

  static auto Run(int argc, char* argv[]) -> int;

 protected:
  auto Init(int argc, char* argv[]) -> bool override;
  void Fini() override;
  void OnTickComplete() override;
  void RegisterWatchers() override;

 private:
  friend class DBAppRollbackTest;

  // ---- Message handlers ---------------------------------------------------
  void OnWriteEntity(const Address& src, Channel* ch, const dbapp::WriteEntity& msg);
  void OnCheckoutEntity(const Address& src, Channel* ch, const dbapp::CheckoutEntity& msg);
  void OnCheckinEntity(const Address& src, Channel* ch, const dbapp::CheckinEntity& msg);
  void OnDeleteEntity(const Address& src, Channel* ch, const dbapp::DeleteEntity& msg);
  void OnLookupEntity(const Address& src, Channel* ch, const dbapp::LookupEntity& msg);
  void OnAbortCheckout(const Address& src, Channel* ch, const dbapp::AbortCheckout& msg);

  // ---- EntityID allocation (BaseApp → DBApp) --------------------------------
  void OnGetEntityIds(const Address& src, Channel* ch, const dbapp::GetEntityIds& msg);
  void OnPutEntityIds(const Address& src, Channel* ch, const dbapp::PutEntityIds& msg);

  // ---- Authentication (LoginApp → DBApp) ----------------------------------
  void OnAuthLogin(const Address& src, Channel* ch, const login::AuthLogin& msg);

  // ---- BaseApp death notification -----------------------------------------
  void OnBaseappDeath(const Address& internal_addr, std::string_view name);

  // ---- Helpers ------------------------------------------------------------
  [[nodiscard]] auto BuildDbConfig() const -> DatabaseConfig;
  auto ResolveReplyChannel(const Address& addr) -> Channel*;

  // ---- State --------------------------------------------------------------
  std::unique_ptr<IDatabase> database_;
  std::unique_ptr<EntityIdAllocator> id_allocator_;
  CheckoutManager checkout_mgr_;
  struct PendingCheckoutRequest {
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

#endif  // ATLAS_SERVER_DBAPP_DBAPP_H_
