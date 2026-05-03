#ifndef ATLAS_LIB_NETWORK_MESSAGE_IDS_H_
#define ATLAS_LIB_NETWORK_MESSAGE_IDS_H_

#include <cstdint>
#include <type_traits>

#include "network/message.h"

// Message IDs are wire-compatible; append only, never re-use or rename.
// Ranges:
//   0     -    99   reserved
//   100   -   199   common
//   1000  -  1099   machined
//   2000  -  2999   BaseApp
//   3000  -  3999   CellApp
//   4000  -  4999   DBApp
//   5000  -  5999   LoginApp
//   6000  -  6999   BaseAppMgr
//   7000  -  7999   CellAppMgr
//   8000  -  8999   DBAppMgr
//   10000 - 19999   external client/server
//   50000 - 59999   C# RPC forwarding

namespace atlas::msg_id {

template <typename E>
[[nodiscard]] constexpr auto Id(E e) -> MessageID {
  static_assert(std::is_enum_v<E>);
  return static_cast<MessageID>(e);
}

enum class Common : uint16_t {
  kHeartbeat = 100,
  kShutdownRequest = 101,
  // Entity-RPC reply (matched in PendingRpcRegistry by request_id).
  kEntityRpcReply = 102,
};

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
  kShutdownTarget = 1030,
};

enum class BaseApp : uint16_t {
  kCreateBase = 2000,
  kCreateBaseFromDb = 2001,
  kAcceptClient = 2002,
  kCellEntityCreated = 2010,
  kCellEntityDestroyed = 2011,
  kCurrentCell = 2012,
  kCellRpcForward = 2013,
  // 2014 reserved.
  kReplicatedDeltaFromCell = 2015,
  kBroadcastRpcFromCell = 2016,
  kReplicatedReliableDeltaFromCell = 2017,
  // CellApp -> BaseApp opaque CELL_DATA snapshot for DB writes and recovery.
  kBackupCellEntity = 2018,
  // CellApp -> BaseApp owner baseline for reliable=false client properties.
  kReplicatedBaselineFromCell = 2019,
  kAuthenticate = 2020,
  kAuthenticateResult = 2021,
  kClientBaseRpc = 2022,
  kClientCellRpc = 2023,
  // BaseApp -> Client owner handoff; old entity_id is no longer channel-bound.
  kEntityTransferred = 2024,
  // BaseApp -> Client gate for the first ClientCellRpc after cell creation.
  kCellReady = 2025,
  // CellAppMgr -> BaseApp remap for Reals hosted on a dead CellApp.
  kCellAppDeath = 2026,
  // Client -> BaseApp delta for baseapp/client_event_seq_gaps_total.
  kClientEventSeqReport = 2027,
  kForceLogoff = 2030,
  kForceLogoffAck = 2031,
};

// CellApp outbound traffic to BaseApp reuses BaseApp IDs.

enum class CellApp : uint16_t {
  kCreateCellEntity = 3000,
  kDestroyCellEntity = 3002,
  // Client -> BaseApp -> CellApp (client-initiated cell RPC, REAL_ONLY,
  // requires Exposed + sourceEntityID validation on arrival).
  kClientCellRpcForward = 3003,
  // Server-internal Base -> CellApp (REAL_ONLY, no Exposed check; the
  // base side is already trusted).
  kInternalCellRpc = 3004,
  kCreateSpace = 3010,
  kDestroySpace = 3011,
  kAvatarUpdate = 3020,
  kEnableWitness = 3021,
  kDisableWitness = 3022,
  kSetAoIRadius = 3023,
  // Inter-CellApp Real/Ghost replication and offload.
  kCreateGhost = 3100,
  kDeleteGhost = 3101,
  kGhostPositionUpdate = 3102,
  kGhostDelta = 3103,
  kGhostSetReal = 3104,
  kGhostSetNextReal = 3105,
  kGhostSnapshotRefresh = 3106,
  kOffloadEntity = 3110,
  kOffloadEntityAck = 3111,
};

// CellAppMgr owns both registration/load and control-plane IDs.

enum class CellAppMgr : uint16_t {
  kRegisterCellApp = 7000,
  kRegisterCellAppAck = 7001,
  kInformCellLoad = 7002,
  kCreateSpaceRequest = 7003,
  kAddCellToSpace = 7004,
  kUpdateGeometry = 7005,
  kShouldOffload = 7006,
  kSpaceCreatedResult = 7007,
};

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

enum class BaseAppMgr : uint16_t {
  kRegisterBaseApp = 6000,
  kRegisterBaseAppAck = 6001,
  kBaseAppReady = 6002,
  kInformLoad = 6003,
  kRegisterGlobalBase = 6010,
  kDeregisterGlobalBase = 6011,
  kGlobalBaseNotification = 6012,
};

#define ATLAS_ASSERT_ID_RANGE(enumerator, lo, hi)                                             \
  static_assert(                                                                              \
      static_cast<uint16_t>(enumerator) >= (lo) && static_cast<uint16_t>(enumerator) <= (hi), \
      #enumerator " is outside its allocated ID range")

ATLAS_ASSERT_ID_RANGE(Common::kHeartbeat, 100, 199);
ATLAS_ASSERT_ID_RANGE(Common::kShutdownRequest, 100, 199);
ATLAS_ASSERT_ID_RANGE(Common::kEntityRpcReply, 100, 199);

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
ATLAS_ASSERT_ID_RANGE(Machined::kShutdownTarget, 1000, 1099);

ATLAS_ASSERT_ID_RANGE(BaseApp::kCreateBase, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kCreateBaseFromDb, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kAcceptClient, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kCellEntityCreated, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kCellEntityDestroyed, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kCurrentCell, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kCellRpcForward, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kReplicatedDeltaFromCell, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kBroadcastRpcFromCell, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kReplicatedReliableDeltaFromCell, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kBackupCellEntity, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kReplicatedBaselineFromCell, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kAuthenticate, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kAuthenticateResult, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kEntityTransferred, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kCellReady, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kCellAppDeath, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kForceLogoff, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::kForceLogoffAck, 2000, 2999);

ATLAS_ASSERT_ID_RANGE(CellApp::kCreateCellEntity, 3000, 3999);
ATLAS_ASSERT_ID_RANGE(CellApp::kDestroyCellEntity, 3000, 3999);
ATLAS_ASSERT_ID_RANGE(CellApp::kClientCellRpcForward, 3000, 3999);
ATLAS_ASSERT_ID_RANGE(CellApp::kInternalCellRpc, 3000, 3999);
ATLAS_ASSERT_ID_RANGE(CellApp::kCreateSpace, 3000, 3999);
ATLAS_ASSERT_ID_RANGE(CellApp::kDestroySpace, 3000, 3999);
ATLAS_ASSERT_ID_RANGE(CellApp::kAvatarUpdate, 3000, 3999);
ATLAS_ASSERT_ID_RANGE(CellApp::kEnableWitness, 3000, 3999);
ATLAS_ASSERT_ID_RANGE(CellApp::kDisableWitness, 3000, 3999);
ATLAS_ASSERT_ID_RANGE(CellApp::kSetAoIRadius, 3000, 3999);
ATLAS_ASSERT_ID_RANGE(CellApp::kCreateGhost, 3000, 3999);
ATLAS_ASSERT_ID_RANGE(CellApp::kDeleteGhost, 3000, 3999);
ATLAS_ASSERT_ID_RANGE(CellApp::kGhostPositionUpdate, 3000, 3999);
ATLAS_ASSERT_ID_RANGE(CellApp::kGhostDelta, 3000, 3999);
ATLAS_ASSERT_ID_RANGE(CellApp::kGhostSetReal, 3000, 3999);
ATLAS_ASSERT_ID_RANGE(CellApp::kGhostSetNextReal, 3000, 3999);
ATLAS_ASSERT_ID_RANGE(CellApp::kGhostSnapshotRefresh, 3000, 3999);
ATLAS_ASSERT_ID_RANGE(CellApp::kOffloadEntity, 3000, 3999);
ATLAS_ASSERT_ID_RANGE(CellApp::kOffloadEntityAck, 3000, 3999);

ATLAS_ASSERT_ID_RANGE(CellAppMgr::kRegisterCellApp, 7000, 7099);
ATLAS_ASSERT_ID_RANGE(CellAppMgr::kRegisterCellAppAck, 7000, 7099);
ATLAS_ASSERT_ID_RANGE(CellAppMgr::kInformCellLoad, 7000, 7099);
ATLAS_ASSERT_ID_RANGE(CellAppMgr::kCreateSpaceRequest, 7000, 7099);
ATLAS_ASSERT_ID_RANGE(CellAppMgr::kAddCellToSpace, 7000, 7099);
ATLAS_ASSERT_ID_RANGE(CellAppMgr::kUpdateGeometry, 7000, 7099);
ATLAS_ASSERT_ID_RANGE(CellAppMgr::kShouldOffload, 7000, 7099);
ATLAS_ASSERT_ID_RANGE(CellAppMgr::kSpaceCreatedResult, 7000, 7099);

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

ATLAS_ASSERT_ID_RANGE(Login::kLoginRequest, 5000, 5999);
ATLAS_ASSERT_ID_RANGE(Login::kLoginResult, 5000, 5999);
ATLAS_ASSERT_ID_RANGE(Login::kAuthLogin, 5000, 5999);
ATLAS_ASSERT_ID_RANGE(Login::kAuthLoginResult, 5000, 5999);
ATLAS_ASSERT_ID_RANGE(Login::kAllocateBaseApp, 5000, 5999);
ATLAS_ASSERT_ID_RANGE(Login::kAllocateBaseAppResult, 5000, 5999);
ATLAS_ASSERT_ID_RANGE(Login::kPrepareLogin, 5000, 5999);
ATLAS_ASSERT_ID_RANGE(Login::kPrepareLoginResult, 5000, 5999);
ATLAS_ASSERT_ID_RANGE(Login::kCancelPrepareLogin, 5000, 5999);

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
