#pragma once

#include "network/address.hpp"
#include "network/message.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace atlas::machined
{

// Process info for service registration
struct ProcessInfo
{
    std::string type;  // "loginapp", "baseapp", etc.
    Address address;
    uint32_t pid{0};
};

// Register a process with machined
struct RegisterMessage
{
    std::string type;
    uint16_t port;
    uint32_t pid;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{1000, "machined::Register", MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write_string(type);
        w.write<uint16_t>(port);
        w.write<uint32_t>(pid);
    }

    static auto deserialize(BinaryReader& r) -> Result<RegisterMessage>
    {
        auto t = r.read_string();
        if (!t)
            return t.error();
        auto p = r.read<uint16_t>();
        if (!p)
            return p.error();
        auto pid = r.read<uint32_t>();
        if (!pid)
            return pid.error();
        return RegisterMessage{std::move(*t), *p, *pid};
    }
};

// Deregister a process from machined
struct DeregisterMessage
{
    std::string type;
    uint32_t pid;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{1001, "machined::Deregister", MessageLengthStyle::Variable,
                                      -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write_string(type);
        w.write<uint32_t>(pid);
    }

    static auto deserialize(BinaryReader& r) -> Result<DeregisterMessage>
    {
        auto t = r.read_string();
        if (!t)
            return t.error();
        auto pid = r.read<uint32_t>();
        if (!pid)
            return pid.error();
        return DeregisterMessage{std::move(*t), *pid};
    }
};

// Query for processes of a given type
struct QueryMessage
{
    std::string type;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{1002, "machined::Query", MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const { w.write_string(type); }

    static auto deserialize(BinaryReader& r) -> Result<QueryMessage>
    {
        auto t = r.read_string();
        if (!t)
            return t.error();
        return QueryMessage{std::move(*t)};
    }
};

// Response to a query
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
            w.write_string(p.type);
            w.write<uint32_t>(p.address.ip());
            w.write<uint16_t>(p.address.port());
            w.write<uint32_t>(p.pid);
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
            auto type = r.read_string();
            if (!type)
                return type.error();
            auto ip = r.read<uint32_t>();
            if (!ip)
                return ip.error();
            auto port = r.read<uint16_t>();
            if (!port)
                return port.error();
            auto pid = r.read<uint32_t>();
            if (!pid)
                return pid.error();
            resp.processes.push_back({std::move(*type), Address(*ip, *port), *pid});
        }
        return resp;
    }
};

static_assert(NetworkMessage<RegisterMessage>);
static_assert(NetworkMessage<DeregisterMessage>);
static_assert(NetworkMessage<QueryMessage>);
static_assert(NetworkMessage<QueryResponse>);

}  // namespace atlas::machined
