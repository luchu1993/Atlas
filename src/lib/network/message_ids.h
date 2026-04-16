#ifndef ATLAS_LIB_NETWORK_MESSAGE_IDS_H_
#define ATLAS_LIB_NETWORK_MESSAGE_IDS_H_

#include <cstdint>
#include <type_traits>

#include "network/message.h"

// ============================================================================
// Central MessageID registry
//
// Every Atlas message ID is defined here as an enum class value.
// This file is the single source of truth — no numeric literals appear
// in individual message headers.
//
// Rules for adding a new message:
//   1. Find the enum class for the owning subsystem below.
//   2. Append a new enumerator at the end of that block.
//   3. Never re-use or rename an existing value (it breaks wire compatibility).
//   4. If the subsystem does not exist yet, add a new enum class and update
//      the range comment table at the top of common_messages.hpp.
//
// Duplicate values within the same enum class are a compile error on all
// major compilers (-Wduplicate-enum / C4369).
// Out-of-range values are caught by the static_assert blocks below.
//
// Range allocation (mirrors the comment in common_messages.hpp):
//   0     –    99   reserved
//   100   –   199   common      (Common)
//   1000  –  1099   machined    (Machined)
//   2000  –  2999   BaseApp     (BaseApp)
//   3000  –  3999   CellApp     (CellApp)   — reserved
//   4000  –  4999   DBApp       (DBApp)
//   5000  –  5999   LoginApp    (Login)
//   6000  –  6999   BaseAppMgr  (BaseAppMgr)
//   7000  –  7999   CellAppMgr  (CellAppMgr) — reserved
//   8000  –  8999   DBAppMgr    (DBAppMgr)   — reserved
//   10000 – 19999   external client ↔ server — reserved
//   50000 – 59999   C# RPC forwarding        — reserved
// ============================================================================

namespace atlas::msg_id {

// ── helpers ──────────────────────────────────────────────────────────────────

// Cast an enum class value to MessageID (uint16_t) for use in MessageDesc.
template <typename E>
[[nodiscard]] constexpr auto Id(E e) -> MessageID {
  static_assert(std::is_enum_v<E>);
  return static_cast<MessageID>(e);
}

// ── Common (100–199) ─────────────────────────────────────────────────────────

enum class Common : uint16_t {
  kHeartbeat = 100,
  kShutdownRequest = 101,
};

// ── Machined (1000–1099) ─────────────────────────────────────────────────────

enum class Machined : uint16_t {
  kRegister = 1000,
  kDeregister = 1001,
  kQuery = 1002,
  kQueryResponse = 1003,
  kRegisterAck = 1004,
  kHeartbeat = 1005,
  kHeartbeatAck = 1006,
  kBirthNotification = 1010,
  kDeathNotification = 1011,
  kListenerRegister = 1012,
  kListenerAck = 1013,
  kWatcherRequest = 1020,
  kWatcherResponse = 1021,
  kWatcherForward = 1022,
  kWatcherReply = 1023,
};

// ── BaseApp (2000–2999) ───────────────────────────────────────────────────────

enum class BaseApp : uint16_t {
  // Internal: BaseAppMgr / CellApp / DBApp → BaseApp
  kCreateBase = 2000,
  kCreateBaseFromDb = 2001,
  kAcceptClient = 2002,
  kCellEntityCreated = 2010,
  kCellEntityDestroyed = 2011,
  kCurrentCell = 2012,
  kCellRpcForward = 2013,
  kSelfRpcFromCell = 2014,
  kReplicatedDeltaFromCell = 2015,
  kBroadcastRpcFromCell = 2016,
  // External: Client ↔ BaseApp
  kAuthenticate = 2020,
  kAuthenticateResult = 2021,
  kClientBaseRpc = 2022,  // Client → BaseApp: exposed base method call
  kClientCellRpc = 2023,  // Client → BaseApp: exposed cell method call (forwarded to CellApp)
                          // Internal: BaseApp ↔ BaseApp
  kForceLogoff = 2030,
  kForceLogoffAck = 2031,
};

// ── DBApp (4000–4999) ─────────────────────────────────────────────────────────

enum class DBApp : uint16_t {
  kWriteEntity = 4000,
  kWriteEntityAck = 4001,
  kCheckoutEntity = 4002,
  kCheckoutEntityAck = 4003,
  kCheckinEntity = 4004,
  kDeleteEntity = 4005,
  kDeleteEntityAck = 4006,
  kLookupEntity = 4007,
  kLookupEntityAck = 4008,
  kAbortCheckout = 4009,
  kAbortCheckoutAck = 4010,
  kGetEntityIds = 4020,
  kGetEntityIdsAck = 4021,
  kPutEntityIds = 4022,
  kPutEntityIdsAck = 4023,
};

// ── LoginApp (5000–5999) ──────────────────────────────────────────────────────

enum class Login : uint16_t {
  kLoginRequest = 5000,
  kLoginResult = 5001,
  kAuthLogin = 5002,
  kAuthLoginResult = 5003,
  kAllocateBaseApp = 5004,
  kAllocateBaseAppResult = 5005,
  kPrepareLogin = 5006,
  kPrepareLoginResult = 5007,
  kCancelPrepareLogin = 5008,
};

// ── BaseAppMgr (6000–6999) ────────────────────────────────────────────────────

enum class BaseAppMgr : uint16_t {
  kRegisterBaseApp = 6000,
  kRegisterBaseAppAck = 6001,
  kBaseAppReady = 6002,
  kInformLoad = 6003,
  kRegisterGlobalBase = 6010,
  kDeregisterGlobalBase = 6011,
  kGlobalBaseNotification = 6012,
};

// ── Range static_asserts ──────────────────────────────────────────────────────
// Catch any value accidentally placed outside its allocated band.

#define ATLAS_ASSERT_ID_RANGE(enumerator, lo, hi)                                             \
  static_assert(                                                                              \
      static_cast<uint16_t>(enumerator) >= (lo) && static_cast<uint16_t>(enumerator) <= (hi), \
      #enumerator " is outside its allocated ID range")

// Common
ATLAS_ASSERT_ID_RANGE(Common::kHeartbeat, 100, 199);
ATLAS_ASSERT_ID_RANGE(Common::kShutdownRequest, 100, 199);

// Machined
ATLAS_ASSERT_ID_RANGE(Machined::kRegister, 1000, 1099);
ATLAS_ASSERT_ID_RANGE(Machined::kDeregister, 1000, 1099);
ATLAS_ASSERT_ID_RANGE(Machined::kQuery, 1000, 1099);
ATLAS_ASSERT_ID_RANGE(Machined::kQueryResponse, 1000, 1099);
ATLAS_ASSERT_ID_RANGE(Machined::kRegisterAck, 1000, 1099);
ATLAS_ASSERT_ID_RANGE(Machined::kHeartbeat, 1000, 1099);
ATLAS_ASSERT_ID_RANGE(Machined::kHeartbeatAck, 1000, 1099);
ATLAS_ASSERT_ID_RANGE(Machined::kBirthNotification, 1000, 1099);
ATLAS_ASSERT_ID_RANGE(Machined::kDeathNotification, 1000, 1099);
ATLAS_ASSERT_ID_RANGE(Machined::kListenerRegister, 1000, 1099);
ATLAS_ASSERT_ID_RANGE(Machined::kListenerAck, 1000, 1099);
ATLAS_ASSERT_ID_RANGE(Machined::kWatcherRequest, 1000, 1099);
ATLAS_ASSERT_ID_RANGE(Machined::kWatcherResponse, 1000, 1099);
ATLAS_ASSERT_ID_RANGE(Machined::kWatcherForward, 1000, 1099);
ATLAS_ASSERT_ID_RANGE(Machined::kWatcherReply, 1000, 1099);

// BaseApp
ATLAS_ASSERT_ID_RANGE(BaseApp::kCreateBase, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kCreateBaseFromDb, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kAcceptClient, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kCellEntityCreated, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kCellEntityDestroyed, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kCurrentCell, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kCellRpcForward, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kSelfRpcFromCell, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kReplicatedDeltaFromCell, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kBroadcastRpcFromCell, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kAuthenticate, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kAuthenticateResult, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kForceLogoff, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kForceLogoffAck, 2000, 2999);

// DBApp
ATLAS_ASSERT_ID_RANGE(DBApp::kWriteEntity, 4000, 4999);
ATLAS_ASSERT_ID_RANGE(DBApp::kWriteEntityAck, 4000, 4999);
ATLAS_ASSERT_ID_RANGE(DBApp::kCheckoutEntity, 4000, 4999);
ATLAS_ASSERT_ID_RANGE(DBApp::kCheckoutEntityAck, 4000, 4999);
ATLAS_ASSERT_ID_RANGE(DBApp::kCheckinEntity, 4000, 4999);
ATLAS_ASSERT_ID_RANGE(DBApp::kDeleteEntity, 4000, 4999);
ATLAS_ASSERT_ID_RANGE(DBApp::kDeleteEntityAck, 4000, 4999);
ATLAS_ASSERT_ID_RANGE(DBApp::kLookupEntity, 4000, 4999);
ATLAS_ASSERT_ID_RANGE(DBApp::kLookupEntityAck, 4000, 4999);
ATLAS_ASSERT_ID_RANGE(DBApp::kAbortCheckout, 4000, 4999);
ATLAS_ASSERT_ID_RANGE(DBApp::kAbortCheckoutAck, 4000, 4999);
ATLAS_ASSERT_ID_RANGE(DBApp::kGetEntityIds, 4000, 4999);
ATLAS_ASSERT_ID_RANGE(DBApp::kGetEntityIdsAck, 4000, 4999);
ATLAS_ASSERT_ID_RANGE(DBApp::kPutEntityIds, 4000, 4999);
ATLAS_ASSERT_ID_RANGE(DBApp::kPutEntityIdsAck, 4000, 4999);

// Login
ATLAS_ASSERT_ID_RANGE(Login::kLoginRequest, 5000, 5999);
ATLAS_ASSERT_ID_RANGE(Login::kLoginResult, 5000, 5999);
ATLAS_ASSERT_ID_RANGE(Login::kAuthLogin, 5000, 5999);
ATLAS_ASSERT_ID_RANGE(Login::kAuthLoginResult, 5000, 5999);
ATLAS_ASSERT_ID_RANGE(Login::kAllocateBaseApp, 5000, 5999);
ATLAS_ASSERT_ID_RANGE(Login::kAllocateBaseAppResult, 5000, 5999);
ATLAS_ASSERT_ID_RANGE(Login::kPrepareLogin, 5000, 5999);
ATLAS_ASSERT_ID_RANGE(Login::kPrepareLoginResult, 5000, 5999);
ATLAS_ASSERT_ID_RANGE(Login::kCancelPrepareLogin, 5000, 5999);

// BaseAppMgr
ATLAS_ASSERT_ID_RANGE(BaseAppMgr::kRegisterBaseApp, 6000, 6999);
ATLAS_ASSERT_ID_RANGE(BaseAppMgr::kRegisterBaseAppAck, 6000, 6999);
ATLAS_ASSERT_ID_RANGE(BaseAppMgr::kBaseAppReady, 6000, 6999);
ATLAS_ASSERT_ID_RANGE(BaseAppMgr::kInformLoad, 6000, 6999);
ATLAS_ASSERT_ID_RANGE(BaseAppMgr::kRegisterGlobalBase, 6000, 6999);
ATLAS_ASSERT_ID_RANGE(BaseAppMgr::kDeregisterGlobalBase, 6000, 6999);
ATLAS_ASSERT_ID_RANGE(BaseAppMgr::kGlobalBaseNotification, 6000, 6999);

#undef ATLAS_ASSERT_ID_RANGE

}  // namespace atlas::msg_id

#endif  // ATLAS_LIB_NETWORK_MESSAGE_IDS_H_
