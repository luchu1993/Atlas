#pragma once

#include "network/address.hpp"
#include "network/message.hpp"
#include "server/server_config.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace atlas::machined
{

// ============================================================================
// ProcessInfo — used in QueryResponse
// ============================================================================

struct ProcessInfo
{
    ProcessType process_type{ProcessType::BaseApp};
    std::string name;
    Address internal_addr;
    Address external_addr;  // {0,0} if N/A
    uint32_t pid{0};
    float load{0.0f};  // 0.0 ~ 1.0, from last heartbeat
};

// ============================================================================
// Protocol version
// ============================================================================

inline constexpr uint8_t kProtocolVersion = 1;

// ============================================================================
// Registration (ID 1000–1001)
// ============================================================================

struct RegisterMessage
{
    uint8_t protocol_version{kProtocolVersion};
    ProcessType process_type{ProcessType::BaseApp};
    std::string name;
    uint16_t internal_port{0};
    uint16_t external_port{0};  // 0 = none
    uint32_t pid{0};

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{1000, "machined::Register", MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write<uint8_t>(protocol_version);
        w.write<uint8_t>(static_cast<uint8_t>(process_type));
        w.write_string(name);
        w.write<uint16_t>(internal_port);
        w.write<uint16_t>(external_port);
        w.write<uint32_t>(pid);
    }

    static auto deserialize(BinaryReader& r) -> Result<RegisterMessage>
    {
        RegisterMessage msg;
        auto ver = r.read<uint8_t>();
        if (!ver)
            return ver.error();
        msg.protocol_version = *ver;
        auto pt = r.read<uint8_t>();
        if (!pt)
            return pt.error();
        msg.process_type = static_cast<ProcessType>(*pt);
        auto name = r.read_string();
        if (!name)
            return name.error();
        msg.name = std::move(*name);
        auto iport = r.read<uint16_t>();
        if (!iport)
            return iport.error();
        msg.internal_port = *iport;
        auto eport = r.read<uint16_t>();
        if (!eport)
            return eport.error();
        msg.external_port = *eport;
        auto pid = r.read<uint32_t>();
        if (!pid)
            return pid.error();
        msg.pid = *pid;
        return msg;
    }
};

struct RegisterAck
{
    bool success{true};
    std::string error_message;
    uint64_t server_time{0};         // machined current time (ms since epoch)
    uint16_t heartbeat_udp_port{0};  // UDP port for heartbeat datagrams (0 = use TCP)

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{1004, "machined::RegisterAck", MessageLengthStyle::Variable,
                                      -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write<uint8_t>(success ? 1 : 0);
        w.write_string(error_message);
        w.write<uint64_t>(server_time);
        w.write<uint16_t>(heartbeat_udp_port);
    }

    static auto deserialize(BinaryReader& r) -> Result<RegisterAck>
    {
        RegisterAck msg;
        auto ok = r.read<uint8_t>();
        if (!ok)
            return ok.error();
        msg.success = (*ok != 0);
        auto err = r.read_string();
        if (!err)
            return err.error();
        msg.error_message = std::move(*err);
        auto ts = r.read<uint64_t>();
        if (!ts)
            return ts.error();
        msg.server_time = *ts;
        // heartbeat_udp_port is optional (older machined sends nothing here)
        auto udp_port = r.read<uint16_t>();
        msg.heartbeat_udp_port = udp_port ? *udp_port : 0;
        return msg;
    }
};

struct DeregisterMessage
{
    ProcessType process_type{ProcessType::BaseApp};
    std::string name;
    uint32_t pid{0};

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{1001, "machined::Deregister", MessageLengthStyle::Variable,
                                      -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write<uint8_t>(static_cast<uint8_t>(process_type));
        w.write_string(name);
        w.write<uint32_t>(pid);
    }

    static auto deserialize(BinaryReader& r) -> Result<DeregisterMessage>
    {
        DeregisterMessage msg;
        auto pt = r.read<uint8_t>();
        if (!pt)
            return pt.error();
        msg.process_type = static_cast<ProcessType>(*pt);
        auto name = r.read_string();
        if (!name)
            return name.error();
        msg.name = std::move(*name);
        auto pid = r.read<uint32_t>();
        if (!pid)
            return pid.error();
        msg.pid = *pid;
        return msg;
    }
};

// ============================================================================
// Query (ID 1002–1003)
// ============================================================================

struct QueryMessage
{
    ProcessType process_type{ProcessType::BaseApp};

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{1002, "machined::Query", MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const { w.write<uint8_t>(static_cast<uint8_t>(process_type)); }

    static auto deserialize(BinaryReader& r) -> Result<QueryMessage>
    {
        QueryMessage msg;
        auto pt = r.read<uint8_t>();
        if (!pt)
            return pt.error();
        msg.process_type = static_cast<ProcessType>(*pt);
        return msg;
    }
};

struct QueryResponse
{
    std::vector<ProcessInfo> processes;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{1003, "machined::QueryResponse", MessageLengthStyle::Variable,
                                      -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write<uint32_t>(static_cast<uint32_t>(processes.size()));
        for (const auto& p : processes)
        {
            w.write<uint8_t>(static_cast<uint8_t>(p.process_type));
            w.write_string(p.name);
            w.write<uint32_t>(p.internal_addr.ip());
            w.write<uint16_t>(p.internal_addr.port());
            w.write<uint32_t>(p.external_addr.ip());
            w.write<uint16_t>(p.external_addr.port());
            w.write<uint32_t>(p.pid);
            w.write<float>(p.load);
        }
    }

    static auto deserialize(BinaryReader& r) -> Result<QueryResponse>
    {
        auto count = r.read<uint32_t>();
        if (!count)
            return count.error();

        constexpr uint32_t kMaxProcesses = 10000;
        if (*count > kMaxProcesses)
        {
            return Error{ErrorCode::InvalidArgument, "QueryResponse: process count exceeds limit"};
        }

        QueryResponse resp;
        resp.processes.reserve(*count);
        for (uint32_t i = 0; i < *count; ++i)
        {
            ProcessInfo p;
            auto pt = r.read<uint8_t>();
            if (!pt)
                return pt.error();
            p.process_type = static_cast<ProcessType>(*pt);
            auto name = r.read_string();
            if (!name)
                return name.error();
            p.name = std::move(*name);
            auto iip = r.read<uint32_t>();
            if (!iip)
                return iip.error();
            auto iport = r.read<uint16_t>();
            if (!iport)
                return iport.error();
            p.internal_addr = Address(*iip, *iport);
            auto eip = r.read<uint32_t>();
            if (!eip)
                return eip.error();
            auto eport = r.read<uint16_t>();
            if (!eport)
                return eport.error();
            p.external_addr = Address(*eip, *eport);
            auto pid = r.read<uint32_t>();
            if (!pid)
                return pid.error();
            p.pid = *pid;
            auto load = r.read<float>();
            if (!load)
                return load.error();
            p.load = *load;
            resp.processes.push_back(std::move(p));
        }
        return resp;
    }
};

// ============================================================================
// Heartbeat (ID 1005–1006)
// ============================================================================

struct HeartbeatMessage
{
    float load{0.0f};  // current load (0.0 ~ 1.0)
    uint32_t entity_count{0};

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{1005, "machined::Heartbeat", MessageLengthStyle::Variable,
                                      -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write<float>(load);
        w.write<uint32_t>(entity_count);
    }

    static auto deserialize(BinaryReader& r) -> Result<HeartbeatMessage>
    {
        HeartbeatMessage msg;
        auto load = r.read<float>();
        if (!load)
            return load.error();
        msg.load = *load;
        auto ec = r.read<uint32_t>();
        if (!ec)
            return ec.error();
        msg.entity_count = *ec;
        return msg;
    }
};

struct HeartbeatAck
{
    uint64_t server_time{0};

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{1006, "machined::HeartbeatAck", MessageLengthStyle::Variable,
                                      -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const { w.write<uint64_t>(server_time); }

    static auto deserialize(BinaryReader& r) -> Result<HeartbeatAck>
    {
        HeartbeatAck msg;
        auto ts = r.read<uint64_t>();
        if (!ts)
            return ts.error();
        msg.server_time = *ts;
        return msg;
    }
};

// ============================================================================
// Birth / Death notifications (ID 1010–1011)
// ============================================================================

struct BirthNotification
{
    ProcessType process_type{ProcessType::BaseApp};
    std::string name;
    Address internal_addr;
    Address external_addr;
    uint32_t pid{0};

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{1010, "machined::BirthNotification",
                                      MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write<uint8_t>(static_cast<uint8_t>(process_type));
        w.write_string(name);
        w.write<uint32_t>(internal_addr.ip());
        w.write<uint16_t>(internal_addr.port());
        w.write<uint32_t>(external_addr.ip());
        w.write<uint16_t>(external_addr.port());
        w.write<uint32_t>(pid);
    }

    static auto deserialize(BinaryReader& r) -> Result<BirthNotification>
    {
        BirthNotification msg;
        auto pt = r.read<uint8_t>();
        if (!pt)
            return pt.error();
        msg.process_type = static_cast<ProcessType>(*pt);
        auto name = r.read_string();
        if (!name)
            return name.error();
        msg.name = std::move(*name);
        auto iip = r.read<uint32_t>();
        if (!iip)
            return iip.error();
        auto iport = r.read<uint16_t>();
        if (!iport)
            return iport.error();
        msg.internal_addr = Address(*iip, *iport);
        auto eip = r.read<uint32_t>();
        if (!eip)
            return eip.error();
        auto eport = r.read<uint16_t>();
        if (!eport)
            return eport.error();
        msg.external_addr = Address(*eip, *eport);
        auto pid = r.read<uint32_t>();
        if (!pid)
            return pid.error();
        msg.pid = *pid;
        return msg;
    }
};

struct DeathNotification
{
    ProcessType process_type{ProcessType::BaseApp};
    std::string name;
    Address internal_addr;
    uint8_t reason{0};  // 0=normal, 1=connection_lost, 2=timeout

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{1011, "machined::DeathNotification",
                                      MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write<uint8_t>(static_cast<uint8_t>(process_type));
        w.write_string(name);
        w.write<uint32_t>(internal_addr.ip());
        w.write<uint16_t>(internal_addr.port());
        w.write<uint8_t>(reason);
    }

    static auto deserialize(BinaryReader& r) -> Result<DeathNotification>
    {
        DeathNotification msg;
        auto pt = r.read<uint8_t>();
        if (!pt)
            return pt.error();
        msg.process_type = static_cast<ProcessType>(*pt);
        auto name = r.read_string();
        if (!name)
            return name.error();
        msg.name = std::move(*name);
        auto iip = r.read<uint32_t>();
        if (!iip)
            return iip.error();
        auto iport = r.read<uint16_t>();
        if (!iport)
            return iport.error();
        msg.internal_addr = Address(*iip, *iport);
        auto reason = r.read<uint8_t>();
        if (!reason)
            return reason.error();
        msg.reason = *reason;
        return msg;
    }
};

// ============================================================================
// Listener registration (ID 1012–1013)
// ============================================================================

enum class ListenerType : uint8_t
{
    Birth = 0,
    Death = 1,
    Both = 2,
};

struct ListenerRegister
{
    ListenerType listener_type{ListenerType::Both};
    ProcessType target_type{ProcessType::BaseApp};

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{1012, "machined::ListenerRegister",
                                      MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write<uint8_t>(static_cast<uint8_t>(listener_type));
        w.write<uint8_t>(static_cast<uint8_t>(target_type));
    }

    static auto deserialize(BinaryReader& r) -> Result<ListenerRegister>
    {
        ListenerRegister msg;
        auto lt = r.read<uint8_t>();
        if (!lt)
            return lt.error();
        msg.listener_type = static_cast<ListenerType>(*lt);
        auto pt = r.read<uint8_t>();
        if (!pt)
            return pt.error();
        msg.target_type = static_cast<ProcessType>(*pt);
        return msg;
    }
};

struct ListenerAck
{
    bool success{true};

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{1013, "machined::ListenerAck", MessageLengthStyle::Variable,
                                      -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const { w.write<uint8_t>(success ? 1 : 0); }

    static auto deserialize(BinaryReader& r) -> Result<ListenerAck>
    {
        ListenerAck msg;
        auto ok = r.read<uint8_t>();
        if (!ok)
            return ok.error();
        msg.success = (*ok != 0);
        return msg;
    }
};

// ============================================================================
// Watcher forwarding (ID 1020–1023)
// ============================================================================

struct WatcherRequest
{
    ProcessType target_type{ProcessType::BaseApp};
    std::string target_name;  // empty = all of that type
    std::string watcher_path;
    uint32_t request_id{0};

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{1020, "machined::WatcherRequest",
                                      MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write<uint8_t>(static_cast<uint8_t>(target_type));
        w.write_string(target_name);
        w.write_string(watcher_path);
        w.write<uint32_t>(request_id);
    }

    static auto deserialize(BinaryReader& r) -> Result<WatcherRequest>
    {
        WatcherRequest msg;
        auto pt = r.read<uint8_t>();
        if (!pt)
            return pt.error();
        msg.target_type = static_cast<ProcessType>(*pt);
        auto tname = r.read_string();
        if (!tname)
            return tname.error();
        msg.target_name = std::move(*tname);
        auto path = r.read_string();
        if (!path)
            return path.error();
        msg.watcher_path = std::move(*path);
        auto rid = r.read<uint32_t>();
        if (!rid)
            return rid.error();
        msg.request_id = *rid;
        return msg;
    }
};

struct WatcherResponse
{
    uint32_t request_id{0};
    bool found{false};
    std::string source_name;
    std::string value;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{1021, "machined::WatcherResponse",
                                      MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write<uint32_t>(request_id);
        w.write<uint8_t>(found ? 1 : 0);
        w.write_string(source_name);
        w.write_string(value);
    }

    static auto deserialize(BinaryReader& r) -> Result<WatcherResponse>
    {
        WatcherResponse msg;
        auto rid = r.read<uint32_t>();
        if (!rid)
            return rid.error();
        msg.request_id = *rid;
        auto found = r.read<uint8_t>();
        if (!found)
            return found.error();
        msg.found = (*found != 0);
        auto sname = r.read_string();
        if (!sname)
            return sname.error();
        msg.source_name = std::move(*sname);
        auto val = r.read_string();
        if (!val)
            return val.error();
        msg.value = std::move(*val);
        return msg;
    }
};

struct WatcherForward
{
    uint32_t request_id{0};
    std::string requester_name;
    std::string watcher_path;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{1022, "machined::WatcherForward",
                                      MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write<uint32_t>(request_id);
        w.write_string(requester_name);
        w.write_string(watcher_path);
    }

    static auto deserialize(BinaryReader& r) -> Result<WatcherForward>
    {
        WatcherForward msg;
        auto rid = r.read<uint32_t>();
        if (!rid)
            return rid.error();
        msg.request_id = *rid;
        auto rname = r.read_string();
        if (!rname)
            return rname.error();
        msg.requester_name = std::move(*rname);
        auto path = r.read_string();
        if (!path)
            return path.error();
        msg.watcher_path = std::move(*path);
        return msg;
    }
};

struct WatcherReply
{
    uint32_t request_id{0};
    bool found{false};
    std::string value;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{1023, "machined::WatcherReply", MessageLengthStyle::Variable,
                                      -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write<uint32_t>(request_id);
        w.write<uint8_t>(found ? 1 : 0);
        w.write_string(value);
    }

    static auto deserialize(BinaryReader& r) -> Result<WatcherReply>
    {
        WatcherReply msg;
        auto rid = r.read<uint32_t>();
        if (!rid)
            return rid.error();
        msg.request_id = *rid;
        auto found = r.read<uint8_t>();
        if (!found)
            return found.error();
        msg.found = (*found != 0);
        auto val = r.read_string();
        if (!val)
            return val.error();
        msg.value = std::move(*val);
        return msg;
    }
};

// ============================================================================
// Static assertions
// ============================================================================

static_assert(NetworkMessage<RegisterMessage>);
static_assert(NetworkMessage<RegisterAck>);
static_assert(NetworkMessage<DeregisterMessage>);
static_assert(NetworkMessage<QueryMessage>);
static_assert(NetworkMessage<QueryResponse>);
static_assert(NetworkMessage<HeartbeatMessage>);
static_assert(NetworkMessage<HeartbeatAck>);
static_assert(NetworkMessage<BirthNotification>);
static_assert(NetworkMessage<DeathNotification>);
static_assert(NetworkMessage<ListenerRegister>);
static_assert(NetworkMessage<ListenerAck>);
static_assert(NetworkMessage<WatcherRequest>);
static_assert(NetworkMessage<WatcherResponse>);
static_assert(NetworkMessage<WatcherForward>);
static_assert(NetworkMessage<WatcherReply>);

}  // namespace atlas::machined
