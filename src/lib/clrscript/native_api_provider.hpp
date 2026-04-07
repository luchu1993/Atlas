#pragma once

#include <cstddef>
#include <cstdint>

namespace atlas
{

// ============================================================================
// INativeApiProvider — per-process implementation of atlas_* export functions
// ============================================================================
//
// The atlas_* C-linkage functions exported by atlas_engine.dll/.so delegate
// to the provider registered for the current process type.  Each server
// process (BaseApp, CellApp, DBApp, …) registers its own implementation
// before initialising ClrHost:
//
//   BaseAppNativeProvider provider(...);
//   atlas::set_native_api_provider(&provider);
//   clr_host.initialize(...);
//
// The provider pointer must remain valid for the lifetime of the process.

class INativeApiProvider
{
public:
    virtual ~INativeApiProvider() = default;

    // ---- Logging --------------------------------------------------------
    // level values match atlas::LogLevel: Debug=1, Info=2, Warning=3,
    // Error=4, Critical=5.  msg is a UTF-8 string of byte-length len.
    virtual void log_message(int32_t level, const char* msg, int32_t len) = 0;

    // ---- Time -----------------------------------------------------------
    virtual double server_time() = 0;  // seconds since epoch
    virtual float delta_time() = 0;    // last frame duration, seconds

    // ---- Process identity -----------------------------------------------
    virtual uint8_t get_process_prefix() = 0;

    // ---- RPC dispatch ---------------------------------------------------
    // payload points to a length-prefixed serialised argument buffer.
    virtual void send_client_rpc(uint32_t entity_id, uint32_t rpc_id, uint8_t target,
                                 const std::byte* payload, int32_t len) = 0;

    virtual void send_cell_rpc(uint32_t entity_id, uint32_t rpc_id, const std::byte* payload,
                               int32_t len) = 0;

    virtual void send_base_rpc(uint32_t entity_id, uint32_t rpc_id, const std::byte* payload,
                               int32_t len) = 0;

    // ---- Entity type registry -------------------------------------------
    // data is a serialised entity-type descriptor.
    virtual void register_entity_type(const std::byte* data, int32_t len) = 0;
    virtual void unregister_all_entity_types() = 0;
};

// Register the provider for this process.  Must be called before
// ClrHost::initialize().  The pointer must remain valid until process exit.
void set_native_api_provider(INativeApiProvider* provider);

// Retrieve the registered provider.  Asserts (and terminates) if none was
// registered — a programming error, not a recoverable runtime failure.
INativeApiProvider& get_native_api_provider();

}  // namespace atlas
