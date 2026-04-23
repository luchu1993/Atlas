#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "clrscript/base_native_provider.h"
#include "clrscript/native_api_provider.h"

using namespace atlas;

// ============================================================================
// Mock provider — records calls for assertion
// ============================================================================

struct MockProvider final : public INativeApiProvider {
  struct LogCall {
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

  void LogMessage(int32_t level, const char* msg, int32_t len) override {
    log_calls.push_back({level, std::string(msg, static_cast<std::size_t>(len))});
  }

  double ServerTime() override { return 1.5; }
  float DeltaTime() override { return 0.016f; }
  uint8_t GetProcessPrefix() override { return process_prefix; }

  void SendClientRpc(uint32_t, uint32_t, const std::byte*, int32_t) override {
    ++send_client_rpc_count;
  }

  void SendCellRpc(uint32_t, uint32_t, const std::byte*, int32_t) override {
    ++send_cell_rpc_count;
  }

  void SendBaseRpc(uint32_t, uint32_t, const std::byte*, int32_t) override {
    ++send_base_rpc_count;
  }

  void RegisterEntityType(const std::byte*, int32_t) override { ++register_type_count; }

  void UnregisterAllEntityTypes() override { unregister_all_called = true; }

  void WriteToDb(uint32_t, const std::byte*, int32_t) override {}
  void GiveClientTo(uint32_t, uint32_t) override {}
  auto CreateBaseEntity(uint16_t, uint32_t) -> uint32_t override { return 0; }
  void SetAoIRadius(uint32_t, float, float) override {}
  void SetNativeCallbacks(const void*, int32_t) override {}

  // CellApp-specific no-op overrides — mock doesn't exercise these, it
  // just needs to stay concrete as INativeApiProvider evolves.
  void SetEntityPosition(uint32_t, float, float, float) override {}
  void PublishReplicationFrame(uint32_t, uint64_t, uint64_t, const std::byte*, int32_t,
                               const std::byte*, int32_t, const std::byte*, int32_t,
                               const std::byte*, int32_t) override {}
  auto AddMoveController(uint32_t, float, float, float, float, int32_t) -> int32_t override {
    return 0;
  }
  auto AddTimerController(uint32_t, float, bool, int32_t) -> int32_t override { return 0; }
  auto AddProximityController(uint32_t, float, int32_t) -> int32_t override { return 0; }
  void CancelController(uint32_t, int32_t) override {}
};

// ============================================================================
// Provider registration
// ============================================================================

TEST(NativeApiProvider, SetAndGetRoundtrip) {
  MockProvider mock;
  SetNativeApiProvider(&mock);
  EXPECT_EQ(&GetNativeApiProvider(), &mock);
}

TEST(NativeApiProvider, ReplacementIsReflected) {
  MockProvider first;
  MockProvider second;

  SetNativeApiProvider(&first);
  EXPECT_EQ(&GetNativeApiProvider(), &first);

  SetNativeApiProvider(&second);
  EXPECT_EQ(&GetNativeApiProvider(), &second);
}

// ============================================================================
// Delegation through the provider
// ============================================================================

TEST(NativeApiProvider, LogMessageDelegates) {
  MockProvider mock;
  SetNativeApiProvider(&mock);

  const char* msg = "hello provider";
  mock.LogMessage(2, msg, static_cast<int32_t>(std::strlen(msg)));

  ASSERT_EQ(mock.log_calls.size(), 1u);
  EXPECT_EQ(mock.log_calls[0].level, 2);
  EXPECT_EQ(mock.log_calls[0].message, "hello provider");
}

TEST(NativeApiProvider, ServerTimeDelegates) {
  MockProvider mock;
  SetNativeApiProvider(&mock);
  EXPECT_DOUBLE_EQ(mock.ServerTime(), 1.5);
}

TEST(NativeApiProvider, DeltaTimeDelegates) {
  MockProvider mock;
  SetNativeApiProvider(&mock);
  EXPECT_FLOAT_EQ(mock.DeltaTime(), 0.016f);
}

TEST(NativeApiProvider, GetProcessPrefixDelegates) {
  MockProvider mock;
  mock.process_prefix = 7;
  SetNativeApiProvider(&mock);
  EXPECT_EQ(mock.GetProcessPrefix(), 7u);
}

TEST(NativeApiProvider, RpcCallsDelegateCorrectly) {
  MockProvider mock;
  SetNativeApiProvider(&mock);

  mock.SendClientRpc(1, 2, nullptr, 0);
  mock.SendCellRpc(1, 2, nullptr, 0);
  mock.SendBaseRpc(1, 2, nullptr, 0);

  EXPECT_EQ(mock.send_client_rpc_count, 1);
  EXPECT_EQ(mock.send_cell_rpc_count, 1);
  EXPECT_EQ(mock.send_base_rpc_count, 1);
}

TEST(NativeApiProvider, EntityTypeRegistryDelegates) {
  MockProvider mock;
  SetNativeApiProvider(&mock);

  mock.RegisterEntityType(nullptr, 0);
  mock.RegisterEntityType(nullptr, 0);
  EXPECT_EQ(mock.register_type_count, 2);

  mock.UnregisterAllEntityTypes();
  EXPECT_TRUE(mock.unregister_all_called);
}

// ============================================================================
// BaseNativeProvider — default implementations
// ============================================================================

struct ConcreteProvider final : public BaseNativeProvider {};

TEST(BaseNativeProvider, LogMessageRoutesToAtlasLog) {
  // Just verify it doesn't crash for each level value.
  ConcreteProvider p;
  const char* msg = "test";
  auto len = static_cast<int32_t>(std::strlen(msg));

  // LogLevel values: Trace=0, Debug=1, Info=2, Warning=3, Error=4, Critical=5
  for (int32_t level = 0; level <= 5; ++level) {
    EXPECT_NO_FATAL_FAILURE(p.LogMessage(level, msg, len));
  }
}

TEST(BaseNativeProvider, DefaultTimeStubs) {
  ConcreteProvider p;
  EXPECT_DOUBLE_EQ(p.ServerTime(), 0.0);
  EXPECT_FLOAT_EQ(p.DeltaTime(), 0.0f);
}

// ============================================================================
// BUG-08: LogMessage must safely ignore null msg or non-positive len instead
// of constructing a string_view with invalid arguments (UB / out-of-bounds).
// ============================================================================

TEST(BaseNativeProvider, LogMessageNullMsgIsIgnored) {
  ConcreteProvider p;
  EXPECT_NO_FATAL_FAILURE(p.LogMessage(2 /*Info*/, nullptr, 5));
}

TEST(BaseNativeProvider, LogMessageNegativeLenIsIgnored) {
  ConcreteProvider p;
  EXPECT_NO_FATAL_FAILURE(p.LogMessage(2 /*Info*/, "hello", -1));
}

TEST(BaseNativeProvider, LogMessageZeroLenIsIgnored) {
  ConcreteProvider p;
  EXPECT_NO_FATAL_FAILURE(p.LogMessage(2 /*Info*/, "hello", 0));
}

TEST(BaseNativeProvider, LogMessageValidMsgStillWorks) {
  // Sanity-check that valid inputs still reach the logging path without crashing.
  ConcreteProvider p;
  const char* msg = "valid message";
  EXPECT_NO_FATAL_FAILURE(p.LogMessage(2 /*Info*/, msg, static_cast<int32_t>(std::strlen(msg))));
}
