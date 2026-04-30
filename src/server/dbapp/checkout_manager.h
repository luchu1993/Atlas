#ifndef ATLAS_SERVER_DBAPP_CHECKOUT_MANAGER_H_
#define ATLAS_SERVER_DBAPP_CHECKOUT_MANAGER_H_

#include <optional>
#include <unordered_map>

#include "db/idatabase.h"
#include "network/address.h"

namespace atlas {

// Guards one live or in-flight checkout per database entity.
class CheckoutManager {
 public:
  enum class CheckoutStatus {
    kSuccess,            // slot was free, caller may proceed
    kAlreadyCheckedOut,  // confirmed owner exists
    kPendingCheckout,    // another request is in-flight
  };

  struct CheckoutResult {
    CheckoutStatus status{CheckoutStatus::kSuccess};
    CheckoutInfo current_owner;  // valid when AlreadyCheckedOut
  };

  [[nodiscard]] auto TryCheckout(DatabaseID dbid, uint16_t type_id, const CheckoutInfo& new_owner)
      -> CheckoutResult;

  void ConfirmCheckout(DatabaseID dbid, uint16_t type_id);

  void ReleaseCheckout(DatabaseID dbid, uint16_t type_id);

  void Checkin(DatabaseID dbid, uint16_t type_id);

  [[nodiscard]] auto GetOwner(DatabaseID dbid, uint16_t type_id) const
      -> std::optional<CheckoutInfo>;

  auto ClearAllFor(const Address& base_addr) -> int;

  [[nodiscard]] auto size() const -> std::size_t { return entries_.size(); }

 private:
  struct Key {
    DatabaseID dbid{0};
    uint16_t type_id{0};

    bool operator==(const Key& o) const = default;
  };

  struct KeyHash {
    auto operator()(const Key& k) const -> std::size_t {
      std::size_t h = static_cast<std::size_t>(k.dbid);
      h ^= static_cast<std::size_t>(k.type_id) * 0x9e3779b97f4a7c15ULL + 0x6c62272e07bb0142ULL +
           (h << 6) + (h >> 2);
      return h;
    }
  };

  enum class State {
    kChecking,   // DB operation in-flight
    kConfirmed,  // Entity is live on BaseApp
  };

  struct Entry {
    CheckoutInfo owner;
    State state{State::kChecking};
  };

  std::unordered_map<Key, Entry, KeyHash> entries_;
};

}  // namespace atlas

#endif  // ATLAS_SERVER_DBAPP_CHECKOUT_MANAGER_H_
