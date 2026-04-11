#pragma once

#include "network/address.hpp"
#include "network/channel.hpp"
#include "server/server_config.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace atlas::machined
{

// ============================================================================
// ProcessEntry — one registered process
// ============================================================================

struct ProcessEntry
{
    ProcessType process_type{ProcessType::BaseApp};
    std::string name;
    Address internal_addr;
    Address external_addr;  // {0,0} if N/A
    uint32_t pid{0};
    float load{0.0f};

    // Backpointer to the live TCP channel.  Null if the process has disconnected
    // but cleanup has not yet run.  MUST be accessed only from the dispatcher thread.
    Channel* channel{nullptr};
};

// ============================================================================
// ProcessRegistry
// ============================================================================

class ProcessRegistry
{
public:
    // Returns false if the entry would violate uniqueness rules:
    //   - (process_type, name) pair must be unique.
    //   - channel must not already be associated with another entry.
    [[nodiscard]] auto register_process(ProcessEntry entry) -> bool;

    // Remove by channel pointer.  Returns the removed entry, or nullopt if not found.
    auto unregister_by_channel(Channel* channel) -> std::optional<ProcessEntry>;

    // Remove by (type, name).  Returns the removed entry, or nullopt if not found.
    auto unregister_by_name(ProcessType type, const std::string& name)
        -> std::optional<ProcessEntry>;

    // Query — returns a snapshot (copies)
    [[nodiscard]] auto find_by_type(ProcessType type) const -> std::vector<ProcessEntry>;
    [[nodiscard]] auto find_by_name(ProcessType type, const std::string& name) const
        -> std::optional<ProcessEntry>;
    [[nodiscard]] auto find_by_channel(Channel* channel) const -> std::optional<ProcessEntry>;

    // Update load for a connected process
    void update_load(Channel* channel, float load, uint32_t entity_count);

    // Look up the TCP channel for a process by its source IP (used to correlate
    // UDP heartbeat datagrams, which arrive on an ephemeral port, with their TCP
    // registration channel).  Returns nullptr if no match.
    [[nodiscard]] auto find_tcp_channel_by_ip(uint32_t ip) const -> Channel*;

    [[nodiscard]] auto size() const -> std::size_t { return entries_.size(); }

    // Iterate all entries (read-only)
    using VisitorFn = std::function<void(const ProcessEntry&)>;
    void for_each(VisitorFn fn) const;

private:
    std::vector<ProcessEntry> entries_;
};

}  // namespace atlas::machined
