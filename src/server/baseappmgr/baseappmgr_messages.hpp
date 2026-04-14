#pragma once

#include "network/address.hpp"
#include "network/message.hpp"
#include "network/message_ids.hpp"
#include "server/entity_types.hpp"

#include <cstdint>
#include <string>

// ============================================================================
// BaseAppMgr messages (IDs 6000–6021)
//
// Directions:
//   BaseApp    → BaseAppMgr : RegisterBaseApp     (6000)
//   BaseAppMgr → BaseApp    : RegisterBaseAppAck  (6001)
//   BaseApp    → BaseAppMgr : BaseAppReady        (6002)
//   BaseApp    → BaseAppMgr : InformLoad          (6003)
//   BaseApp    → BaseAppMgr : RegisterGlobalBase  (6010)
//   BaseApp    → BaseAppMgr : DeregisterGlobalBase(6011)
//   BaseAppMgr → BaseApp    : GlobalBaseNotification (6012)
//   BaseApp    → BaseAppMgr : RequestEntityIdRange (6020)
//   BaseAppMgr → BaseApp    : RequestEntityIdRangeAck (6021)
// ============================================================================

namespace atlas::baseappmgr
{

// ============================================================================
// RegisterBaseApp  (BaseApp → BaseAppMgr, ID 6000)
// ============================================================================

struct RegisterBaseApp
{
    Address internal_addr;
    Address external_addr;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::BaseAppMgr::RegisterBaseApp),
                                      "baseappmgr::RegisterBaseApp", MessageLengthStyle::Fixed,
                                      static_cast<int>((sizeof(uint32_t) + sizeof(uint16_t)) * 2)};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write(internal_addr.ip());
        w.write(internal_addr.port());
        w.write(external_addr.ip());
        w.write(external_addr.port());
    }

    static auto deserialize(BinaryReader& r) -> Result<RegisterBaseApp>
    {
        auto iip = r.read<uint32_t>();
        auto iport = r.read<uint16_t>();
        auto eip = r.read<uint32_t>();
        auto eport = r.read<uint16_t>();
        if (!iip || !iport || !eip || !eport)
            return Error{ErrorCode::InvalidArgument, "RegisterBaseApp: truncated"};
        RegisterBaseApp msg;
        msg.internal_addr = Address(*iip, *iport);
        msg.external_addr = Address(*eip, *eport);
        return msg;
    }
};
static_assert(NetworkMessage<RegisterBaseApp>);

// ============================================================================
// RegisterBaseAppAck  (BaseAppMgr → BaseApp, ID 6001)
// ============================================================================

struct RegisterBaseAppAck
{
    bool success{false};
    uint32_t app_id{0};
    uint64_t game_time{0};

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{
            msg_id::id(msg_id::BaseAppMgr::RegisterBaseAppAck), "baseappmgr::RegisterBaseAppAck",
            MessageLengthStyle::Fixed,
            static_cast<int>(sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint64_t))};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write(static_cast<uint8_t>(success ? 1 : 0));
        w.write(app_id);
        w.write(game_time);
    }

    static auto deserialize(BinaryReader& r) -> Result<RegisterBaseAppAck>
    {
        auto ok = r.read<uint8_t>();
        auto aid = r.read<uint32_t>();
        auto gt = r.read<uint64_t>();
        if (!ok || !aid || !gt)
            return Error{ErrorCode::InvalidArgument, "RegisterBaseAppAck: truncated"};
        RegisterBaseAppAck msg;
        msg.success = (*ok != 0);
        msg.app_id = *aid;
        msg.game_time = *gt;
        return msg;
    }
};
static_assert(NetworkMessage<RegisterBaseAppAck>);

// ============================================================================
// BaseAppReady  (BaseApp → BaseAppMgr, ID 6002)
// ============================================================================

struct BaseAppReady
{
    uint32_t app_id{0};

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::BaseAppMgr::BaseAppReady),
                                      "baseappmgr::BaseAppReady", MessageLengthStyle::Fixed,
                                      static_cast<int>(sizeof(uint32_t))};
        return desc;
    }

    void serialize(BinaryWriter& w) const { w.write(app_id); }

    static auto deserialize(BinaryReader& r) -> Result<BaseAppReady>
    {
        auto aid = r.read<uint32_t>();
        if (!aid)
            return Error{ErrorCode::InvalidArgument, "BaseAppReady: truncated"};
        BaseAppReady msg;
        msg.app_id = *aid;
        return msg;
    }
};
static_assert(NetworkMessage<BaseAppReady>);

// ============================================================================
// InformLoad  (BaseApp → BaseAppMgr, ID 6003)
// ============================================================================

struct InformLoad
{
    uint32_t app_id{0};
    float load{0.0f};
    uint32_t entity_count{0};
    uint32_t proxy_count{0};
    uint32_t pending_prepare_count{0};
    uint32_t pending_force_logoff_count{0};
    uint32_t detached_proxy_count{0};
    uint32_t logoff_in_flight_count{0};
    uint32_t deferred_login_count{0};

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{
            6003, "baseappmgr::InformLoad", MessageLengthStyle::Fixed,
            static_cast<int>(sizeof(uint32_t) + sizeof(float) + sizeof(uint32_t) * 7)};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write(app_id);
        w.write(load);
        w.write(entity_count);
        w.write(proxy_count);
        w.write(pending_prepare_count);
        w.write(pending_force_logoff_count);
        w.write(detached_proxy_count);
        w.write(logoff_in_flight_count);
        w.write(deferred_login_count);
    }

    static auto deserialize(BinaryReader& r) -> Result<InformLoad>
    {
        auto aid = r.read<uint32_t>();
        auto ld = r.read<float>();
        auto ec = r.read<uint32_t>();
        auto pc = r.read<uint32_t>();
        auto ppc = r.read<uint32_t>();
        auto pfl = r.read<uint32_t>();
        auto dpc = r.read<uint32_t>();
        auto lif = r.read<uint32_t>();
        auto dlc = r.read<uint32_t>();
        if (!aid || !ld || !ec || !pc || !ppc || !pfl || !dpc || !lif || !dlc)
            return Error{ErrorCode::InvalidArgument, "InformLoad: truncated"};
        InformLoad msg;
        msg.app_id = *aid;
        msg.load = *ld;
        msg.entity_count = *ec;
        msg.proxy_count = *pc;
        msg.pending_prepare_count = *ppc;
        msg.pending_force_logoff_count = *pfl;
        msg.detached_proxy_count = *dpc;
        msg.logoff_in_flight_count = *lif;
        msg.deferred_login_count = *dlc;
        return msg;
    }
};
static_assert(NetworkMessage<InformLoad>);

// ============================================================================
// RegisterGlobalBase  (BaseApp → BaseAppMgr, ID 6010)
// ============================================================================

struct RegisterGlobalBase
{
    std::string key;
    EntityID entity_id{kInvalidEntityID};
    uint16_t type_id{0};

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::BaseAppMgr::RegisterGlobalBase),
                                      "baseappmgr::RegisterGlobalBase",
                                      MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write_string(key);
        w.write(entity_id);
        w.write(type_id);
    }

    static auto deserialize(BinaryReader& r) -> Result<RegisterGlobalBase>
    {
        auto k = r.read_string();
        auto eid = r.read<uint32_t>();
        auto ti = r.read<uint16_t>();
        if (!k || !eid || !ti)
            return Error{ErrorCode::InvalidArgument, "RegisterGlobalBase: truncated"};
        RegisterGlobalBase msg;
        msg.key = std::move(*k);
        msg.entity_id = *eid;
        msg.type_id = *ti;
        return msg;
    }
};
static_assert(NetworkMessage<RegisterGlobalBase>);

// ============================================================================
// DeregisterGlobalBase  (BaseApp → BaseAppMgr, ID 6011)
// ============================================================================

struct DeregisterGlobalBase
{
    std::string key;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::BaseAppMgr::DeregisterGlobalBase),
                                      "baseappmgr::DeregisterGlobalBase",
                                      MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const { w.write_string(key); }

    static auto deserialize(BinaryReader& r) -> Result<DeregisterGlobalBase>
    {
        auto k = r.read_string();
        if (!k)
            return Error{ErrorCode::InvalidArgument, "DeregisterGlobalBase: truncated"};
        DeregisterGlobalBase msg;
        msg.key = std::move(*k);
        return msg;
    }
};
static_assert(NetworkMessage<DeregisterGlobalBase>);

// ============================================================================
// GlobalBaseNotification  (BaseAppMgr → BaseApp, ID 6012)
// ============================================================================

struct GlobalBaseNotification
{
    std::string key;
    Address base_addr;
    EntityID entity_id{kInvalidEntityID};
    uint16_t type_id{0};
    bool added{true};  // true=registered, false=deregistered

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::BaseAppMgr::GlobalBaseNotification),
                                      "baseappmgr::GlobalBaseNotification",
                                      MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write_string(key);
        w.write(base_addr.ip());
        w.write(base_addr.port());
        w.write(entity_id);
        w.write(type_id);
        w.write(static_cast<uint8_t>(added ? 1 : 0));
    }

    static auto deserialize(BinaryReader& r) -> Result<GlobalBaseNotification>
    {
        auto k = r.read_string();
        auto ip = r.read<uint32_t>();
        auto port = r.read<uint16_t>();
        auto eid = r.read<uint32_t>();
        auto ti = r.read<uint16_t>();
        auto add = r.read<uint8_t>();
        if (!k || !ip || !port || !eid || !ti || !add)
            return Error{ErrorCode::InvalidArgument, "GlobalBaseNotification: truncated"};
        GlobalBaseNotification msg;
        msg.key = std::move(*k);
        msg.base_addr = Address(*ip, *port);
        msg.entity_id = *eid;
        msg.type_id = *ti;
        msg.added = (*add != 0);
        return msg;
    }
};
static_assert(NetworkMessage<GlobalBaseNotification>);

}  // namespace atlas::baseappmgr
