#include "baseapp/baseapp.hpp"
#include "db/idatabase.hpp"
#include "dbapp/dbapp.hpp"
#include "loginapp/loginapp.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <utility>

namespace atlas
{

class LoginRollbackTest : public ::testing::Test
{
protected:
    LoginRollbackTest()
        : dispatcher_("login_rollback"), internal_network_(dispatcher_),
          external_network_(dispatcher_), app_(dispatcher_, internal_network_, external_network_)
    {
    }

    void register_watchers() { app_.register_watchers(); }

    void seed_pending_login(uint32_t request_id, std::string username)
    {
        LoginApp::PendingLogin pending;
        pending.request_id = request_id;
        pending.username = std::move(username);
        pending.created_at = Clock::now();
        app_.pending_by_username_[pending.username] = pending.request_id;
        app_.pending_[pending.request_id] = pending;
    }

    void abandon_pending_login(uint32_t request_id)
    {
        auto it = app_.pending_.find(request_id);
        ASSERT_NE(it, app_.pending_.end());
        app_.abandon_pending_login(it);
    }

    auto is_canceled(uint32_t request_id) const -> bool
    {
        return app_.canceled_requests_.contains(request_id);
    }

    auto pending_empty() const -> bool { return app_.pending_.empty(); }
    auto pending_by_username_empty() const -> bool { return app_.pending_by_username_.empty(); }
    auto abandoned_total() const -> uint64_t { return app_.abandoned_login_total_; }
    auto watcher(std::string_view path) -> std::optional<std::string>
    {
        return app_.watcher_registry().get(path);
    }

    EventDispatcher dispatcher_;
    NetworkInterface internal_network_;
    NetworkInterface external_network_;
    LoginApp app_;
};

TEST_F(LoginRollbackTest, AbandonPendingLoginMarksCanceledAndUpdatesWatchers)
{
    register_watchers();
    seed_pending_login(42, "tester");

    abandon_pending_login(42);

    EXPECT_TRUE(pending_empty());
    EXPECT_TRUE(pending_by_username_empty());
    ASSERT_TRUE(is_canceled(42));
    EXPECT_EQ(abandoned_total(), 1u);
    EXPECT_EQ(watcher("loginapp/abandoned_login_total").value_or(""), "1");
    EXPECT_EQ(watcher("loginapp/canceled_request_count").value_or(""), "1");
}

class BaseAppRollbackTest : public ::testing::Test
{
protected:
    BaseAppRollbackTest()
        : dispatcher_("baseapp_rollback"), internal_network_(dispatcher_),
          external_network_(dispatcher_), app_(dispatcher_, internal_network_, external_network_)
    {
    }

    void register_watchers() { app_.register_watchers(); }

    void seed_expired_prepared_login(uint32_t login_request_id, EntityID entity_id)
    {
        app_.prepared_login_entities_[login_request_id] = BaseApp::PreparedLoginEntity{
            entity_id, 1234, 7,
            Clock::now() - BaseApp::kPreparedLoginTimeout - std::chrono::seconds(1)};
        app_.prepared_login_requests_by_entity_[entity_id] = login_request_id;
    }

    void cleanup_expired() { app_.cleanup_expired_pending_requests(); }

    auto has_prepared(uint32_t login_request_id) const -> bool
    {
        return app_.prepared_login_entities_.contains(login_request_id);
    }

    auto has_prepared_entity(EntityID entity_id) const -> bool
    {
        return app_.prepared_login_requests_by_entity_.contains(entity_id);
    }

    auto prepared_timeout_total() const -> uint64_t { return app_.prepared_login_timeout_total_; }
    auto watcher(std::string_view path) -> std::optional<std::string>
    {
        return app_.watcher_registry().get(path);
    }

    EventDispatcher dispatcher_;
    NetworkInterface internal_network_;
    NetworkInterface external_network_;
    BaseApp app_;
};

TEST_F(BaseAppRollbackTest, PreparedLoginTimeoutRollsBackPreparedState)
{
    register_watchers();
    seed_expired_prepared_login(77, 901);

    cleanup_expired();

    EXPECT_FALSE(has_prepared(77));
    EXPECT_FALSE(has_prepared_entity(901));
    EXPECT_EQ(prepared_timeout_total(), 1u);
    EXPECT_EQ(watcher("baseapp/prepared_login_timeout_total").value_or(""), "1");
    EXPECT_EQ(watcher("baseapp/canceled_checkout_count").value_or(""), "0");
}

class FakeDatabase final : public IDatabase
{
public:
    Result<void> startup(const DatabaseConfig&, const EntityDefRegistry&) override { return {}; }
    void shutdown() override {}

    void put_entity(DatabaseID, uint16_t, WriteFlags, std::span<const std::byte>,
                    const std::string&, std::function<void(PutResult)>) override
    {
        ADD_FAILURE() << "put_entity should not be called in this test";
    }

    void get_entity(DatabaseID, uint16_t, std::function<void(GetResult)>) override
    {
        ADD_FAILURE() << "get_entity should not be called in this test";
    }

    void del_entity(DatabaseID, uint16_t, std::function<void(DelResult)>) override
    {
        ADD_FAILURE() << "del_entity should not be called in this test";
    }

    void lookup_by_name(uint16_t, const std::string&, std::function<void(LookupResult)>) override
    {
        ADD_FAILURE() << "lookup_by_name should not be called in this test";
    }

    void checkout_entity(DatabaseID, uint16_t, const CheckoutInfo&,
                         std::function<void(GetResult)> callback) override
    {
        checkout_callback = std::move(callback);
    }

    void checkout_entity_by_name(uint16_t, const std::string&, const CheckoutInfo&,
                                 std::function<void(GetResult)> callback) override
    {
        checkout_callback = std::move(callback);
    }

    void clear_checkout(DatabaseID dbid, uint16_t type_id, std::function<void(bool)> callback)
        override
    {
        ++clear_checkout_calls;
        last_cleared = std::make_pair(dbid, type_id);
        callback(true);
    }

    void clear_checkouts_for_address(const Address&, std::function<void(int)> callback) override
    {
        callback(0);
    }

    void mark_checkout_cleared(DatabaseID dbid, uint16_t type_id) override
    {
        ++mark_checkout_cleared_calls;
        last_cleared = std::make_pair(dbid, type_id);
    }

    void get_auto_load_entities(std::function<void(std::vector<EntityData>)> callback) override
    {
        callback({});
    }

    void set_auto_load(DatabaseID, uint16_t, bool) override {}
    void process_results() override {}

    std::function<void(GetResult)> checkout_callback;
    int clear_checkout_calls{0};
    int mark_checkout_cleared_calls{0};
    std::optional<std::pair<DatabaseID, uint16_t>> last_cleared;
};

class DBAppRollbackTest : public ::testing::Test
{
protected:
    DBAppRollbackTest()
        : dispatcher_("dbapp_rollback"), network_(dispatcher_), app_(dispatcher_, network_)
    {
        app_.database_ = std::make_unique<FakeDatabase>();
    }

    auto db() -> FakeDatabase&
    {
        return *static_cast<FakeDatabase*>(app_.database_.get());
    }

    void register_watchers() { app_.register_watchers(); }

    void start_checkout(uint32_t request_id, DatabaseID dbid, uint16_t type_id)
    {
        dbapp::CheckoutEntity checkout;
        checkout.request_id = request_id;
        checkout.dbid = dbid;
        checkout.type_id = type_id;
        app_.on_checkout_entity(Address("127.0.0.1", 30001), reinterpret_cast<Channel*>(1),
                                checkout);
    }

    void abort_checkout(uint32_t request_id, DatabaseID dbid, uint16_t type_id)
    {
        dbapp::AbortCheckout abort;
        abort.request_id = request_id;
        abort.dbid = dbid;
        abort.type_id = type_id;
        app_.on_abort_checkout(Address("127.0.0.1", 30001), reinterpret_cast<Channel*>(1), abort);
    }

    void complete_checkout_success(DatabaseID dbid)
    {
        ASSERT_TRUE(db().checkout_callback);
        GetResult result;
        result.success = true;
        result.data.dbid = dbid;
        db().checkout_callback(std::move(result));
    }

    auto pending_request_contains(uint32_t request_id) const -> bool
    {
        return app_.pending_checkout_requests_.contains(request_id);
    }

    auto pending_request_canceled(uint32_t request_id) const -> bool
    {
        return app_.pending_checkout_requests_.at(request_id).canceled;
    }

    auto pending_request_cleared_dbid(uint32_t request_id) const -> DatabaseID
    {
        return app_.pending_checkout_requests_.at(request_id).cleared_dbid;
    }

    auto checkout_owner(DatabaseID dbid, uint16_t type_id) const -> std::optional<CheckoutInfo>
    {
        return app_.checkout_mgr_.get_owner(dbid, type_id);
    }

    auto abort_total() const -> uint64_t { return app_.abort_checkout_total_; }
    auto abort_pending_total() const -> uint64_t { return app_.abort_checkout_pending_hit_total_; }
    auto abort_late_total() const -> uint64_t { return app_.abort_checkout_late_hit_total_; }
    auto watcher(std::string_view path) -> std::optional<std::string>
    {
        return app_.watcher_registry().get(path);
    }

    EventDispatcher dispatcher_;
    NetworkInterface network_;
    DBApp app_;
};

TEST_F(DBAppRollbackTest, AbortCheckoutIsIdempotentWhileRequestIsPending)
{
    register_watchers();
    start_checkout(1001, 555, 9);
    ASSERT_TRUE(db().checkout_callback);
    ASSERT_TRUE(pending_request_contains(1001));

    abort_checkout(1001, 555, 9);
    abort_checkout(1001, 555, 9);

    EXPECT_EQ(db().mark_checkout_cleared_calls, 1);
    ASSERT_TRUE(pending_request_contains(1001));
    EXPECT_TRUE(pending_request_canceled(1001));
    EXPECT_EQ(pending_request_cleared_dbid(1001), 555);

    complete_checkout_success(555);

    EXPECT_FALSE(pending_request_contains(1001));
    EXPECT_EQ(db().mark_checkout_cleared_calls, 1);
    EXPECT_FALSE(checkout_owner(555, 9).has_value());
    EXPECT_EQ(abort_total(), 2u);
    EXPECT_EQ(abort_pending_total(), 2u);
    EXPECT_EQ(abort_late_total(), 0u);
    EXPECT_EQ(watcher("dbapp/abort_checkout_total").value_or(""), "2");
}

}  // namespace atlas
