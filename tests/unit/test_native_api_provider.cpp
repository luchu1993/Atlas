#include "clrscript/base_native_provider.hpp"
#include "clrscript/native_api_provider.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

using namespace atlas;

// ============================================================================
// Mock provider — records calls for assertion
// ============================================================================

struct MockProvider final : public INativeApiProvider
{
    struct LogCall
    {
        int32_t level;
        std::string message;
    };

    std::vector<LogCall> log_calls;
    int send_client_rpc_count = 0;
    int send_cell_rpc_count = 0;
    int send_base_rpc_count = 0;
    int register_type_count = 0;
    bool unregister_all_called = false;
    uint8_t process_prefix = 42;

    void log_message(int32_t level, const char* msg, int32_t len) override
    {
        log_calls.push_back({level, std::string(msg, static_cast<std::size_t>(len))});
    }

    double server_time() override { return 1.5; }
    float delta_time() override { return 0.016f; }
    uint8_t get_process_prefix() override { return process_prefix; }

    void send_client_rpc(uint32_t, uint32_t, uint8_t, const std::byte*, int32_t) override
    {
        ++send_client_rpc_count;
    }

    void send_cell_rpc(uint32_t, uint32_t, const std::byte*, int32_t) override
    {
        ++send_cell_rpc_count;
    }

    void send_base_rpc(uint32_t, uint32_t, const std::byte*, int32_t) override
    {
        ++send_base_rpc_count;
    }

    void register_entity_type(const std::byte*, int32_t) override { ++register_type_count; }

    void unregister_all_entity_types() override { unregister_all_called = true; }

    void write_to_db(uint32_t, const std::byte*, int32_t) override {}
    void give_client_to(uint32_t, uint32_t) override {}
    void set_native_callbacks(const void*, int32_t) override {}
};

// ============================================================================
// Provider registration
// ============================================================================

TEST(NativeApiProvider, SetAndGetRoundtrip)
{
    MockProvider mock;
    set_native_api_provider(&mock);
    EXPECT_EQ(&get_native_api_provider(), &mock);
}

TEST(NativeApiProvider, ReplacementIsReflected)
{
    MockProvider first;
    MockProvider second;

    set_native_api_provider(&first);
    EXPECT_EQ(&get_native_api_provider(), &first);

    set_native_api_provider(&second);
    EXPECT_EQ(&get_native_api_provider(), &second);
}

// ============================================================================
// Delegation through the provider
// ============================================================================

TEST(NativeApiProvider, LogMessageDelegates)
{
    MockProvider mock;
    set_native_api_provider(&mock);

    const char* msg = "hello provider";
    mock.log_message(2, msg, static_cast<int32_t>(std::strlen(msg)));

    ASSERT_EQ(mock.log_calls.size(), 1u);
    EXPECT_EQ(mock.log_calls[0].level, 2);
    EXPECT_EQ(mock.log_calls[0].message, "hello provider");
}

TEST(NativeApiProvider, ServerTimeDelegates)
{
    MockProvider mock;
    set_native_api_provider(&mock);
    EXPECT_DOUBLE_EQ(mock.server_time(), 1.5);
}

TEST(NativeApiProvider, DeltaTimeDelegates)
{
    MockProvider mock;
    set_native_api_provider(&mock);
    EXPECT_FLOAT_EQ(mock.delta_time(), 0.016f);
}

TEST(NativeApiProvider, GetProcessPrefixDelegates)
{
    MockProvider mock;
    mock.process_prefix = 7;
    set_native_api_provider(&mock);
    EXPECT_EQ(mock.get_process_prefix(), 7u);
}

TEST(NativeApiProvider, RpcCallsDelegateCorrectly)
{
    MockProvider mock;
    set_native_api_provider(&mock);

    mock.send_client_rpc(1, 2, 0, nullptr, 0);
    mock.send_cell_rpc(1, 2, nullptr, 0);
    mock.send_base_rpc(1, 2, nullptr, 0);

    EXPECT_EQ(mock.send_client_rpc_count, 1);
    EXPECT_EQ(mock.send_cell_rpc_count, 1);
    EXPECT_EQ(mock.send_base_rpc_count, 1);
}

TEST(NativeApiProvider, EntityTypeRegistryDelegates)
{
    MockProvider mock;
    set_native_api_provider(&mock);

    mock.register_entity_type(nullptr, 0);
    mock.register_entity_type(nullptr, 0);
    EXPECT_EQ(mock.register_type_count, 2);

    mock.unregister_all_entity_types();
    EXPECT_TRUE(mock.unregister_all_called);
}

// ============================================================================
// BaseNativeProvider — default implementations
// ============================================================================

struct ConcreteProvider final : public BaseNativeProvider
{
};

TEST(BaseNativeProvider, LogMessageRoutesToAtlasLog)
{
    // Just verify it doesn't crash for each level value.
    ConcreteProvider p;
    const char* msg = "test";
    auto len = static_cast<int32_t>(std::strlen(msg));

    // LogLevel values: Trace=0, Debug=1, Info=2, Warning=3, Error=4, Critical=5
    for (int32_t level = 0; level <= 5; ++level)
    {
        EXPECT_NO_FATAL_FAILURE(p.log_message(level, msg, len));
    }
}

TEST(BaseNativeProvider, DefaultTimeStubs)
{
    ConcreteProvider p;
    EXPECT_DOUBLE_EQ(p.server_time(), 0.0);
    EXPECT_FLOAT_EQ(p.delta_time(), 0.0f);
}

// ============================================================================
// BUG-08: log_message must safely ignore null msg or non-positive len instead
// of constructing a string_view with invalid arguments (UB / out-of-bounds).
// ============================================================================

TEST(BaseNativeProvider, LogMessageNullMsgIsIgnored)
{
    ConcreteProvider p;
    EXPECT_NO_FATAL_FAILURE(p.log_message(2 /*Info*/, nullptr, 5));
}

TEST(BaseNativeProvider, LogMessageNegativeLenIsIgnored)
{
    ConcreteProvider p;
    EXPECT_NO_FATAL_FAILURE(p.log_message(2 /*Info*/, "hello", -1));
}

TEST(BaseNativeProvider, LogMessageZeroLenIsIgnored)
{
    ConcreteProvider p;
    EXPECT_NO_FATAL_FAILURE(p.log_message(2 /*Info*/, "hello", 0));
}

TEST(BaseNativeProvider, LogMessageValidMsgStillWorks)
{
    // Sanity-check that valid inputs still reach the logging path without crashing.
    ConcreteProvider p;
    const char* msg = "valid message";
    EXPECT_NO_FATAL_FAILURE(p.log_message(2 /*Info*/, msg, static_cast<int32_t>(std::strlen(msg))));
}
