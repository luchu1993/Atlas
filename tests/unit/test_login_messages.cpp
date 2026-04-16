#include <gtest/gtest.h>

#include "loginapp/login_messages.h"
#include "serialization/binary_stream.h"

using namespace atlas;
using namespace atlas::login;

namespace {

// Helper to round-trip serialize/deserialize
template <typename Msg>
auto round_trip(const Msg& msg) -> Msg {
  BinaryWriter w;
  msg.Serialize(w);
  BinaryReader r(w.data());
  auto result = Msg::Deserialize(r);
  EXPECT_TRUE(result.HasValue()) << "deserialize failed";
  return result.ValueOr(Msg{});
}

TEST(LoginMessages, LoginRequest_RoundTrip) {
  LoginRequest msg;
  msg.username = "player1";
  msg.password_hash = "abc123def456";

  auto out = round_trip(msg);
  EXPECT_EQ(out.username, msg.username);
  EXPECT_EQ(out.password_hash, msg.password_hash);
}

TEST(LoginMessages, LoginResult_Success_RoundTrip) {
  LoginResult msg;
  msg.status = LoginStatus::kSuccess;
  msg.session_key = SessionKey::Generate();
  msg.baseapp_addr = Address(0x7F000001u, 20100);

  auto out = round_trip(msg);
  EXPECT_EQ(out.status, LoginStatus::kSuccess);
  EXPECT_EQ(out.session_key, msg.session_key);
  EXPECT_EQ(out.baseapp_addr.Port(), msg.baseapp_addr.Port());
}

TEST(LoginMessages, LoginResult_Error_RoundTrip) {
  LoginResult msg;
  msg.status = LoginStatus::kInvalidCredentials;
  msg.error_message = "bad password";

  auto out = round_trip(msg);
  EXPECT_EQ(out.status, LoginStatus::kInvalidCredentials);
  EXPECT_EQ(out.error_message, "bad password");
  EXPECT_TRUE(out.session_key.IsZero());
}

TEST(LoginMessages, AuthLogin_RoundTrip) {
  AuthLogin msg;
  msg.request_id = 42;
  msg.username = "admin";
  msg.password_hash = "hash";
  msg.auto_create = true;

  auto out = round_trip(msg);
  EXPECT_EQ(out.request_id, 42u);
  EXPECT_EQ(out.username, "admin");
  EXPECT_TRUE(out.auto_create);
}

TEST(LoginMessages, AuthLoginResult_RoundTrip) {
  AuthLoginResult msg;
  msg.request_id = 99;
  msg.success = true;
  msg.status = LoginStatus::kSuccess;
  msg.dbid = 12345;
  msg.type_id = 7;

  auto out = round_trip(msg);
  EXPECT_EQ(out.request_id, 99u);
  EXPECT_TRUE(out.success);
  EXPECT_EQ(out.dbid, 12345);
  EXPECT_EQ(out.type_id, 7u);
}

TEST(LoginMessages, AllocateBaseApp_RoundTrip) {
  AllocateBaseApp msg;
  msg.request_id = 1;
  msg.type_id = 3;
  msg.dbid = 99;

  auto out = round_trip(msg);
  EXPECT_EQ(out.request_id, 1u);
  EXPECT_EQ(out.type_id, 3u);
  EXPECT_EQ(out.dbid, 99);
}

TEST(LoginMessages, AllocateBaseAppResult_RoundTrip) {
  AllocateBaseAppResult msg;
  msg.request_id = 5;
  msg.success = true;
  msg.internal_addr = Address(0x01020304u, 9000);
  msg.external_addr = Address(0x0A000001u, 20100);

  auto out = round_trip(msg);
  EXPECT_TRUE(out.success);
  EXPECT_EQ(out.internal_addr.Port(), 9000u);
  EXPECT_EQ(out.external_addr.Port(), 20100u);
}

TEST(LoginMessages, PrepareLogin_RoundTrip) {
  PrepareLogin msg;
  msg.request_id = 8;
  msg.type_id = 2;
  msg.dbid = 777;
  msg.session_key = SessionKey::Generate();
  msg.client_addr = Address(0x7F000001u, 55000);

  auto out = round_trip(msg);
  EXPECT_EQ(out.request_id, 8u);
  EXPECT_EQ(out.dbid, 777);
  EXPECT_EQ(out.session_key, msg.session_key);
  EXPECT_EQ(out.client_addr.Port(), 55000u);
}

TEST(LoginMessages, PrepareLoginResult_RoundTrip) {
  PrepareLoginResult msg;
  msg.request_id = 10;
  msg.success = true;
  msg.entity_id = 5001;
  msg.error = "";

  auto out = round_trip(msg);
  EXPECT_TRUE(out.success);
  EXPECT_EQ(out.entity_id, 5001u);
}

TEST(LoginMessages, CancelPrepareLogin_RoundTrip) {
  CancelPrepareLogin msg;
  msg.request_id = 77;
  msg.dbid = 1234;

  auto out = round_trip(msg);
  EXPECT_EQ(out.request_id, 77u);
  EXPECT_EQ(out.dbid, 1234);
}

TEST(LoginMessages, SessionKey_Generate_Unique) {
  auto k1 = SessionKey::Generate();
  auto k2 = SessionKey::Generate();
  EXPECT_NE(k1, k2) << "Two generated SessionKeys should differ";
}

TEST(LoginMessages, SessionKey_IsZero) {
  SessionKey zero{};
  EXPECT_TRUE(zero.IsZero());
  auto k = SessionKey::Generate();
  EXPECT_FALSE(k.IsZero());
}

}  // namespace
