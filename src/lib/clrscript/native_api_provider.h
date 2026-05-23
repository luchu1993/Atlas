#ifndef ATLAS_LIB_CLRSCRIPT_NATIVE_API_PROVIDER_H_
#define ATLAS_LIB_CLRSCRIPT_NATIVE_API_PROVIDER_H_

#include <cstddef>
#include <cstdint>

namespace atlas {

enum class RpcTarget : uint8_t {
  kOwner = 0,   // entity's own bound client only
  kOthers = 1,  // every witness in AoI except the entity's owner
  kAll = 2,     // every witness in AoI (owner + others)
};

// Provider pointer must outlive the process; AtlasNative* exports delegate here.
class INativeApiProvider {
 public:
  virtual ~INativeApiProvider() = default;

  // level == atlas::LogLevel; msg is UTF-8 with byte length len.
  virtual void LogMessage(int32_t level, const char* msg, int32_t len) = 0;

  virtual double ServerTime() = 0;  // seconds since epoch
  virtual float DeltaTime() = 0;    // last frame duration, seconds

  virtual uint8_t GetProcessPrefix() = 0;
  virtual void ReportScriptTick(uint32_t /*entity_id*/, uint64_t /*elapsed_us*/) {}

  // Only CellAppNativeProvider supports kOthers / kAll; other hosts
  // log + no-op when target != kOwner.
  virtual void SendClientRpc(uint32_t entity_id, uint32_t rpc_id, RpcTarget target,
                             const std::byte* payload, int32_t len, uint64_t trace_id) = 0;

  virtual void SendCellRpc(uint32_t entity_id, uint32_t rpc_id, const std::byte* payload,
                           int32_t len, uint64_t trace_id) = 0;

  virtual void SendBaseRpc(uint32_t entity_id, uint32_t rpc_id, const std::byte* payload,
                           int32_t len, uint64_t trace_id) = 0;

  virtual void RegisterEntityType(const std::byte* data, int32_t len) = 0;
  virtual void UnregisterAllEntityTypes() = 0;

  // Struct descriptors must be registered before entity types that reference them.
  virtual void RegisterStruct(const std::byte* data, int32_t len) = 0;

  // Register after structs, before any entity slot table referencing this id.
  // Default no-op so headless / test providers don't have to override.
  virtual void RegisterComponent(const std::byte* /*data*/, int32_t /*len*/) {}

  // 32-byte SHA-256 of the entity-def surface; BaseApp compares to
  // LoginRequest.entity_def_digest to bounce mismatched builds.
  virtual void SetEntityDefDigest(const std::byte* data, int32_t len) = 0;

  virtual void WriteToDb(uint32_t entity_id, const std::byte* entity_data, int32_t len) = 0;

  virtual void GiveClientTo(uint32_t src_entity_id, uint32_t dest_entity_id) = 0;

  // Empty name unregisters. Registration also fires a CreateSpaceRequest so
  // CellAppMgr stamps the type onto AddCellToSpace for the primary host, which
  // auto-spawns the entity via its EntityDefRegistry. BaseApp-only.
  virtual void SetSpaceMasterType(uint32_t space_id, const char* name, int32_t len) = 0;

  virtual auto CreateBaseEntity(uint16_t type_id, uint32_t space_id) -> uint32_t = 0;

  // Cell-only entity: lives on this CellApp, no Base counterpart, no DB row.
  // Returns 0 on failure. Non-trivial only on CellAppNativeProvider.
  virtual auto CreateLocalCellEntity(uint16_t type_id, uint32_t space_id, float pos_x, float pos_y,
                                     float pos_z, float dir_x, float dir_y, float dir_z,
                                     bool on_ground) -> uint32_t = 0;

  // Refuses base-owned entities.
  virtual void DestroyCellEntity(uint32_t entity_id) = 0;

  // Base-side router; cell assigns the id, so the call is async and returns
  // only whether the routing message was dispatched.
  virtual auto RequestSpawnCellOnly(uint16_t type_id, uint32_t space_id, float pos_x, float pos_y,
                                    float pos_z, float dir_x, float dir_y, float dir_z,
                                    bool on_ground) -> bool = 0;

  virtual void SetAoIRadius(uint32_t entity_id, float radius, float hysteresis) = 0;

  // Owner-authoritative SpaceData write — fans out to peer cellapps and
  // local witnesses. Only CellAppNativeProvider implements non-trivially.
  virtual void SetSpaceData(uint32_t space_id, uint16_t key_id, const std::byte* value,
                            int32_t len) = 0;
  virtual void RemoveSpaceData(uint32_t space_id, uint16_t key_id) = 0;

  // Returns the entity's owning space id, or 0 if unknown. Cellapp-only;
  // base/login processes inherit the 0 default.
  virtual auto GetEntitySpaceId(uint32_t entity_id) -> uint32_t = 0;

  // Packed function pointer table for C++ -> C# calls.
  virtual void SetNativeCallbacks(const void* native_callbacks, int32_t len) = 0;

  // CellApp owns position authority; other hosts inherit no-op + error log.
  virtual void SetEntityPosition(uint32_t entity_id, float x, float y, float z) = 0;

  // Witness reads dir off CellEntity for the volatile envelope.
  virtual void SetEntityDirection(uint32_t entity_id, float x, float y, float z) = 0;

  // Spawn-time readback so C# can adopt the CellEntity's constructed state.
  virtual void GetEntityPosition(uint32_t entity_id, float& x, float& y, float& z) = 0;
  virtual void GetEntityDirection(uint32_t entity_id, float& x, float& y, float& z) = 0;

  // Owner/other snapshot pointers consumed only when has_event is true; pass
  // nullptr/0 when the event stream is empty. Runtime allocates seqs from
  // CellEntity::replication_state_, so script never owns the counter.
  virtual void PublishReplicationFrame(uint32_t entity_id, bool has_event, bool has_volatile,
                                       const std::byte* owner_snap, int32_t owner_snap_len,
                                       const std::byte* other_snap, int32_t other_snap_len,
                                       const std::byte* owner_delta, int32_t owner_delta_len,
                                       const std::byte* other_delta, int32_t other_delta_len) = 0;

  // Returned controller_id is opaque; pass back to CancelController.
  virtual auto AddMoveController(uint32_t entity_id, float dest_x, float dest_y, float dest_z,
                                 float speed, int32_t user_arg) -> int32_t = 0;
  virtual auto AddTimerController(uint32_t entity_id, float interval, bool repeat, int32_t user_arg)
      -> int32_t = 0;
  virtual auto AddProximityController(uint32_t entity_id, float range, int32_t user_arg)
      -> int32_t = 0;
  virtual void CancelController(uint32_t entity_id, int32_t controller_id) = 0;

  virtual void ReportClientEventSeqGap(uint32_t entity_id, uint32_t gap_delta) = 0;

  // 0 on failure; default no-op for hosts without PendingRpcRegistry.
  virtual auto CoroRegisterPending(uint16_t /*reply_id*/, uint32_t /*request_id*/,
                                   int32_t /*timeout_ms*/, intptr_t /*managed_handle*/)
      -> uint64_t {
    return 0;
  }
  virtual void CoroCancelPending(uint64_t /*handle*/) {}

  // reply_channel == 0 means in-process — drop and let the await time out.
  virtual void SendEntityRpcSuccess(intptr_t /*reply_channel*/, uint32_t /*request_id*/,
                                    const std::byte* /*body*/, int32_t /*len*/) {}
  virtual void SendEntityRpcFailure(intptr_t /*reply_channel*/, uint32_t /*request_id*/,
                                    int32_t /*error_code*/, const char* /*msg*/,
                                    int32_t /*msg_len*/) {}
};

void SetNativeApiProvider(INativeApiProvider* provider);

// Missing provider is a programming error, not a recoverable runtime failure.
INativeApiProvider& GetNativeApiProvider();

}  // namespace atlas

#endif  // ATLAS_LIB_CLRSCRIPT_NATIVE_API_PROVIDER_H_
