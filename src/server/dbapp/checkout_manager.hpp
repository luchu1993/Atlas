#pragma once

#include "db/idatabase.hpp"
#include "network/address.hpp"

#include <optional>
#include <unordered_map>

namespace atlas
{

// ============================================================================
// CheckoutManager — in-memory entity checkout tracker
//
// Mirrors BigWorld's LogOnRecordsCache.
// Prevents the same entity from being loaded by two BaseApps simultaneously.
//
// States:
//   Checking  — a checkout request is in-flight (DB operation pending).
//                Concurrent requests for the same entity are rejected.
//   Confirmed — the DB operation succeeded; entity is live on a BaseApp.
// ============================================================================

class CheckoutManager
{
public:
    // ---- Result types -------------------------------------------------------

    enum class CheckoutStatus
    {
        Success,            // slot was free, caller may proceed
        AlreadyCheckedOut,  // confirmed owner exists
        PendingCheckout,    // another request is in-flight
    };

    struct CheckoutResult
    {
        CheckoutStatus status{CheckoutStatus::Success};
        CheckoutInfo current_owner;  // valid when AlreadyCheckedOut
    };

    // ---- API ----------------------------------------------------------------

    /// Atomically claim the checkout slot.
    /// Returns Success if the slot was free (state set to Checking).
    /// Call confirm_checkout() after the DB operation succeeds.
    /// Call release_checkout() if the DB operation fails.
    [[nodiscard]] auto try_checkout(DatabaseID dbid, uint16_t type_id,
                                    const CheckoutInfo& new_owner) -> CheckoutResult;

    /// Promote a Checking entry to Confirmed after the DB operation succeeds.
    void confirm_checkout(DatabaseID dbid, uint16_t type_id);

    /// Remove a Checking entry if the DB operation failed (rollback).
    void release_checkout(DatabaseID dbid, uint16_t type_id);

    /// Remove the entry (entity going offline).
    void checkin(DatabaseID dbid, uint16_t type_id);

    /// Query current owner (nullopt if not checked out).
    [[nodiscard]] auto get_owner(DatabaseID dbid, uint16_t type_id) const
        -> std::optional<CheckoutInfo>;

    /// Remove all entries owned by a dead BaseApp.
    /// Returns number of entries removed.
    auto clear_all_for(const Address& base_addr) -> int;

    /// Total number of checked-out entries (Checking + Confirmed).
    [[nodiscard]] auto size() const -> std::size_t { return entries_.size(); }

private:
    struct Key
    {
        DatabaseID dbid{0};
        uint16_t type_id{0};

        bool operator==(const Key& o) const = default;
    };

    struct KeyHash
    {
        auto operator()(const Key& k) const -> std::size_t
        {
            // Combine dbid (64-bit) and type_id (16-bit) into a single hash.
            std::size_t h = static_cast<std::size_t>(k.dbid);
            h ^= static_cast<std::size_t>(k.type_id) * 0x9e3779b97f4a7c15ULL +
                 0x6c62272e07bb0142ULL + (h << 6) + (h >> 2);
            return h;
        }
    };

    enum class State
    {
        Checking,   // DB operation in-flight
        Confirmed,  // Entity is live on BaseApp
    };

    struct Entry
    {
        CheckoutInfo owner;
        State state{State::Checking};
    };

    std::unordered_map<Key, Entry, KeyHash> entries_;
};

}  // namespace atlas
