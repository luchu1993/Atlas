#pragma once

#include "network/message.hpp"

#include <cstdint>
#include <type_traits>

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

namespace atlas::msg_id
{

// ── helpers ──────────────────────────────────────────────────────────────────

// Cast an enum class value to MessageID (uint16_t) for use in MessageDesc.
template <typename E>
[[nodiscard]] constexpr auto id(E e) -> MessageID
{
    static_assert(std::is_enum_v<E>);
    return static_cast<MessageID>(e);
}

// ── Common (100–199) ─────────────────────────────────────────────────────────

enum class Common : uint16_t
{
    Heartbeat = 100,
    ShutdownRequest = 101,
};

// ── Machined (1000–1099) ─────────────────────────────────────────────────────

enum class Machined : uint16_t
{
    Register = 1000,
    Deregister = 1001,
    Query = 1002,
    QueryResponse = 1003,
    RegisterAck = 1004,
    Heartbeat = 1005,
    HeartbeatAck = 1006,
    BirthNotification = 1010,
    DeathNotification = 1011,
    ListenerRegister = 1012,
    ListenerAck = 1013,
    WatcherRequest = 1020,
    WatcherResponse = 1021,
    WatcherForward = 1022,
    WatcherReply = 1023,
};

// ── BaseApp (2000–2999) ───────────────────────────────────────────────────────

enum class BaseApp : uint16_t
{
    // Internal: BaseAppMgr / CellApp / DBApp → BaseApp
    CreateBase = 2000,
    CreateBaseFromDB = 2001,
    AcceptClient = 2002,
    CellEntityCreated = 2010,
    CellEntityDestroyed = 2011,
    CurrentCell = 2012,
    CellRpcForward = 2013,
    SelfRpcFromCell = 2014,
    ReplicatedDeltaFromCell = 2015,
    BroadcastRpcFromCell = 2016,
    // External: Client ↔ BaseApp
    Authenticate = 2020,
    AuthenticateResult = 2021,
    ClientBaseRpc = 2022,  // Client → BaseApp: exposed base method call
    ClientCellRpc = 2023,  // Client → BaseApp: exposed cell method call (forwarded to CellApp)
                           // Internal: BaseApp ↔ BaseApp
    ForceLogoff = 2030,
    ForceLogoffAck = 2031,
};

// ── DBApp (4000–4999) ─────────────────────────────────────────────────────────

enum class DBApp : uint16_t
{
    WriteEntity = 4000,
    WriteEntityAck = 4001,
    CheckoutEntity = 4002,
    CheckoutEntityAck = 4003,
    CheckinEntity = 4004,
    DeleteEntity = 4005,
    DeleteEntityAck = 4006,
    LookupEntity = 4007,
    LookupEntityAck = 4008,
    AbortCheckout = 4009,
    AbortCheckoutAck = 4010,
    GetEntityIds = 4020,
    GetEntityIdsAck = 4021,
    PutEntityIds = 4022,
    PutEntityIdsAck = 4023,
};

// ── LoginApp (5000–5999) ──────────────────────────────────────────────────────

enum class Login : uint16_t
{
    LoginRequest = 5000,
    LoginResult = 5001,
    AuthLogin = 5002,
    AuthLoginResult = 5003,
    AllocateBaseApp = 5004,
    AllocateBaseAppResult = 5005,
    PrepareLogin = 5006,
    PrepareLoginResult = 5007,
    CancelPrepareLogin = 5008,
};

// ── BaseAppMgr (6000–6999) ────────────────────────────────────────────────────

enum class BaseAppMgr : uint16_t
{
    RegisterBaseApp = 6000,
    RegisterBaseAppAck = 6001,
    BaseAppReady = 6002,
    InformLoad = 6003,
    RegisterGlobalBase = 6010,
    DeregisterGlobalBase = 6011,
    GlobalBaseNotification = 6012,
};

// ── Range static_asserts ──────────────────────────────────────────────────────
// Catch any value accidentally placed outside its allocated band.

#define ATLAS_ASSERT_ID_RANGE(enumerator, lo, hi)                                               \
    static_assert(                                                                              \
        static_cast<uint16_t>(enumerator) >= (lo) && static_cast<uint16_t>(enumerator) <= (hi), \
        #enumerator " is outside its allocated ID range")

// Common
ATLAS_ASSERT_ID_RANGE(Common::Heartbeat, 100, 199);
ATLAS_ASSERT_ID_RANGE(Common::ShutdownRequest, 100, 199);

// Machined
ATLAS_ASSERT_ID_RANGE(Machined::Register, 1000, 1099);
ATLAS_ASSERT_ID_RANGE(Machined::Deregister, 1000, 1099);
ATLAS_ASSERT_ID_RANGE(Machined::Query, 1000, 1099);
ATLAS_ASSERT_ID_RANGE(Machined::QueryResponse, 1000, 1099);
ATLAS_ASSERT_ID_RANGE(Machined::RegisterAck, 1000, 1099);
ATLAS_ASSERT_ID_RANGE(Machined::Heartbeat, 1000, 1099);
ATLAS_ASSERT_ID_RANGE(Machined::HeartbeatAck, 1000, 1099);
ATLAS_ASSERT_ID_RANGE(Machined::BirthNotification, 1000, 1099);
ATLAS_ASSERT_ID_RANGE(Machined::DeathNotification, 1000, 1099);
ATLAS_ASSERT_ID_RANGE(Machined::ListenerRegister, 1000, 1099);
ATLAS_ASSERT_ID_RANGE(Machined::ListenerAck, 1000, 1099);
ATLAS_ASSERT_ID_RANGE(Machined::WatcherRequest, 1000, 1099);
ATLAS_ASSERT_ID_RANGE(Machined::WatcherResponse, 1000, 1099);
ATLAS_ASSERT_ID_RANGE(Machined::WatcherForward, 1000, 1099);
ATLAS_ASSERT_ID_RANGE(Machined::WatcherReply, 1000, 1099);

// BaseApp
ATLAS_ASSERT_ID_RANGE(BaseApp::CreateBase, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::CreateBaseFromDB, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::AcceptClient, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::CellEntityCreated, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::CellEntityDestroyed, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::CurrentCell, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::CellRpcForward, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::SelfRpcFromCell, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::ReplicatedDeltaFromCell, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::BroadcastRpcFromCell, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::Authenticate, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::AuthenticateResult, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::ForceLogoff, 2000, 2999);
ATLAS_ASSERT_ID_RANGE(BaseApp::ForceLogoffAck, 2000, 2999);

// DBApp
ATLAS_ASSERT_ID_RANGE(DBApp::WriteEntity, 4000, 4999);
ATLAS_ASSERT_ID_RANGE(DBApp::WriteEntityAck, 4000, 4999);
ATLAS_ASSERT_ID_RANGE(DBApp::CheckoutEntity, 4000, 4999);
ATLAS_ASSERT_ID_RANGE(DBApp::CheckoutEntityAck, 4000, 4999);
ATLAS_ASSERT_ID_RANGE(DBApp::CheckinEntity, 4000, 4999);
ATLAS_ASSERT_ID_RANGE(DBApp::DeleteEntity, 4000, 4999);
ATLAS_ASSERT_ID_RANGE(DBApp::DeleteEntityAck, 4000, 4999);
ATLAS_ASSERT_ID_RANGE(DBApp::LookupEntity, 4000, 4999);
ATLAS_ASSERT_ID_RANGE(DBApp::LookupEntityAck, 4000, 4999);
ATLAS_ASSERT_ID_RANGE(DBApp::AbortCheckout, 4000, 4999);
ATLAS_ASSERT_ID_RANGE(DBApp::AbortCheckoutAck, 4000, 4999);
ATLAS_ASSERT_ID_RANGE(DBApp::GetEntityIds, 4000, 4999);
ATLAS_ASSERT_ID_RANGE(DBApp::GetEntityIdsAck, 4000, 4999);
ATLAS_ASSERT_ID_RANGE(DBApp::PutEntityIds, 4000, 4999);
ATLAS_ASSERT_ID_RANGE(DBApp::PutEntityIdsAck, 4000, 4999);

// Login
ATLAS_ASSERT_ID_RANGE(Login::LoginRequest, 5000, 5999);
ATLAS_ASSERT_ID_RANGE(Login::LoginResult, 5000, 5999);
ATLAS_ASSERT_ID_RANGE(Login::AuthLogin, 5000, 5999);
ATLAS_ASSERT_ID_RANGE(Login::AuthLoginResult, 5000, 5999);
ATLAS_ASSERT_ID_RANGE(Login::AllocateBaseApp, 5000, 5999);
ATLAS_ASSERT_ID_RANGE(Login::AllocateBaseAppResult, 5000, 5999);
ATLAS_ASSERT_ID_RANGE(Login::PrepareLogin, 5000, 5999);
ATLAS_ASSERT_ID_RANGE(Login::PrepareLoginResult, 5000, 5999);
ATLAS_ASSERT_ID_RANGE(Login::CancelPrepareLogin, 5000, 5999);

// BaseAppMgr
ATLAS_ASSERT_ID_RANGE(BaseAppMgr::RegisterBaseApp, 6000, 6999);
ATLAS_ASSERT_ID_RANGE(BaseAppMgr::RegisterBaseAppAck, 6000, 6999);
ATLAS_ASSERT_ID_RANGE(BaseAppMgr::BaseAppReady, 6000, 6999);
ATLAS_ASSERT_ID_RANGE(BaseAppMgr::InformLoad, 6000, 6999);
ATLAS_ASSERT_ID_RANGE(BaseAppMgr::RegisterGlobalBase, 6000, 6999);
ATLAS_ASSERT_ID_RANGE(BaseAppMgr::DeregisterGlobalBase, 6000, 6999);
ATLAS_ASSERT_ID_RANGE(BaseAppMgr::GlobalBaseNotification, 6000, 6999);

#undef ATLAS_ASSERT_ID_RANGE

}  // namespace atlas::msg_id
