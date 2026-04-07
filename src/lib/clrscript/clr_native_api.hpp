#pragma once

#include "clrscript/clr_export.hpp"

#include <cstddef>
#include <cstdint>

// ============================================================================
// atlas_* C-linkage export functions
// ============================================================================
//
// These functions are exported by atlas_engine.dll/.so.  C# calls them via
// [LibraryImport("atlas_engine")].
//
// Each function delegates to the INativeApiProvider registered for the
// current process type.  They are implemented in clr_native_api.cpp which is
// compiled exclusively into the atlas_engine shared library.
//
// Naming convention: atlas_ prefix + snake_case, matching the C# entrypoints.

// ---- Logging ----------------------------------------------------------------

ATLAS_NATIVE_API void atlas_log_message(int32_t level, const char* msg, int32_t len);

// ---- Time -------------------------------------------------------------------

ATLAS_NATIVE_API double atlas_server_time();
ATLAS_NATIVE_API float atlas_delta_time();

// ---- Process identity -------------------------------------------------------

ATLAS_NATIVE_API uint8_t atlas_get_process_prefix();

// ---- RPC dispatch -----------------------------------------------------------

ATLAS_NATIVE_API void atlas_send_client_rpc(uint32_t entity_id, uint32_t rpc_id, uint8_t target,
                                            const uint8_t* payload, int32_t len);

ATLAS_NATIVE_API void atlas_send_cell_rpc(uint32_t entity_id, uint32_t rpc_id,
                                          const uint8_t* payload, int32_t len);

ATLAS_NATIVE_API void atlas_send_base_rpc(uint32_t entity_id, uint32_t rpc_id,
                                          const uint8_t* payload, int32_t len);

// ---- Entity type registry ---------------------------------------------------

ATLAS_NATIVE_API void atlas_register_entity_type(const uint8_t* data, int32_t len);
ATLAS_NATIVE_API void atlas_unregister_all_entity_types();
