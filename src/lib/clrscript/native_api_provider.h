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

// The Atlas* C-linkage functions exported by atlas_engine.dll/.so delegate
// to the provider registered for the current process type.
// The provider pointer must remain valid for the lifetime of the process.
class INativeApiProvider {
 public:
  virtual ~INativeApiProvider() = default;

  // level values match atlas::LogLevel: Debug=1, Info=2, Warning=3,
  // Error=4, Critical=5.  msg is a UTF-8 string of byte-length len.
  virtual void LogMessage(int32_t level, const char* msg, int32_t len) = 0;

  virtual double ServerTime() = 0;  // seconds since epoch
  virtual float DeltaTime() = 0;    // last frame duration, seconds

  virtual uint8_t GetProcessPrefix() = 0;

  // payload points to a length-prefixed serialised argument buffer.
  // target selects which subset of clients receives the call (BigWorld-
  // style scopes; only CellAppNativeProvider supports
  // kOthers/kAll; other process types log + no-op when target != kOwner.
  virtual void SendClientRpc(uint32_t entity_id, uint32_t rpc_id, RpcTarget target,
                             const std::byte* payload, int32_t len) = 0;

  virtual void SendCellRpc(uint32_t entity_id, uint32_t rpc_id, const std::byte* payload,
                           int32_t len) = 0;

  virtual void SendBaseRpc(uint32_t entity_id, uint32_t rpc_id, const std::byte* payload,
                           int32_t len) = 0;

  virtual void RegisterEntityType(const std::byte* data, int32_t len) = 0;
  virtual void UnregisterAllEntityTypes() = 0;

  // Struct descriptors must be registered before entity types that reference them.
  virtual void RegisterStruct(const std::byte* data, int32_t len) = 0;

  virtual void WriteToDb(uint32_t entity_id, const std::byte* entity_data, int32_t len) = 0;

  virtual void GiveClientTo(uint32_t src_entity_id, uint32_t dest_entity_id) = 0;

  virtual auto CreateBaseEntity(uint16_t type_id, uint32_t space_id) -> uint32_t = 0;

  virtual void SetAoIRadius(uint32_t entity_id, float radius, float hysteresis) = 0;

  // Packed function pointer table for C++ -> C# calls.
  virtual void SetNativeCallbacks(const void* native_callbacks, int32_t len) = 0;

  // Only CellApp implements these non-trivially; other processes inherit
  // BaseNativeProvider's default no-op + error log. BaseApp might
  // implement SetEntityPosition in the future for the "predict own
  // client" case but for now only CellApp owns position authority.

  virtual void SetEntityPosition(uint32_t entity_id, float x, float y, float z) = 0;

  // C# hands the cell layer a per-tick replication frame. Owner/other
  // snapshot pointers are only consumed when event_seq > 0 (see
  // CellEntity::PublishReplicationFrame); callers pass nullptr/0 when
  // the event stream has nothing this tick.
  virtual void PublishReplicationFrame(uint32_t entity_id, uint64_t event_seq,
                                       uint64_t volatile_seq, const std::byte* owner_snap,
                                       int32_t owner_snap_len, const std::byte* other_snap,
                                       int32_t other_snap_len, const std::byte* owner_delta,
                                       int32_t owner_delta_len, const std::byte* other_delta,
                                       int32_t other_delta_len) = 0;

  // Controller registration; C# scripts create movement / timer /
  // proximity controllers via these calls. The returned controller_id
  // is opaque to the caller; it's passed back to CancelController to
  // tear the controller down early.
  virtual auto AddMoveController(uint32_t entity_id, float dest_x, float dest_y, float dest_z,
                                 float speed, int32_t user_arg) -> int32_t = 0;
  virtual auto AddTimerController(uint32_t entity_id, float interval, bool repeat, int32_t user_arg)
      -> int32_t = 0;
  virtual auto AddProximityController(uint32_t entity_id, float range, int32_t user_arg)
      -> int32_t = 0;
  virtual void CancelController(uint32_t entity_id, int32_t controller_id) = 0;

  virtual void ReportClientEventSeqGap(uint32_t entity_id, uint32_t gap_delta) = 0;
};

void SetNativeApiProvider(INativeApiProvider* provider);

// Missing provider is a programming error, not a recoverable runtime failure.
INativeApiProvider& GetNativeApiProvider();

}  // namespace atlas

#endif  // ATLAS_LIB_CLRSCRIPT_NATIVE_API_PROVIDER_H_
