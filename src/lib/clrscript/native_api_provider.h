#ifndef ATLAS_LIB_CLRSCRIPT_NATIVE_API_PROVIDER_H_
#define ATLAS_LIB_CLRSCRIPT_NATIVE_API_PROVIDER_H_

#include <cstddef>
#include <cstdint>

namespace atlas {

// ============================================================================
// INativeApiProvider — per-process implementation of atlas_* export functions
// ============================================================================
//
// The Atlas* C-linkage functions exported by atlas_engine.dll/.so delegate
// to the provider registered for the current process type.  Each server
// process (BaseApp, CellApp, DBApp, …) registers its own implementation
// before initialising ClrHost:
//
//   BaseAppNativeProvider provider(...);
//   atlas::SetNativeApiProvider(&provider);
//   clr_host.Initialize(...);
//
// The provider pointer must remain valid for the lifetime of the process.

class INativeApiProvider {
 public:
  virtual ~INativeApiProvider() = default;

  // ---- Logging --------------------------------------------------------
  // level values match atlas::LogLevel: Debug=1, Info=2, Warning=3,
  // Error=4, Critical=5.  msg is a UTF-8 string of byte-length len.
  virtual void LogMessage(int32_t level, const char* msg, int32_t len) = 0;

  // ---- Time -----------------------------------------------------------
  virtual double ServerTime() = 0;  // seconds since epoch
  virtual float DeltaTime() = 0;    // last frame duration, seconds

  // ---- Process identity -----------------------------------------------
  virtual uint8_t GetProcessPrefix() = 0;

  // ---- RPC dispatch ---------------------------------------------------
  // payload points to a length-prefixed serialised argument buffer.
  virtual void SendClientRpc(uint32_t entity_id, uint32_t rpc_id, const std::byte* payload,
                             int32_t len) = 0;

  virtual void SendCellRpc(uint32_t entity_id, uint32_t rpc_id, const std::byte* payload,
                           int32_t len) = 0;

  virtual void SendBaseRpc(uint32_t entity_id, uint32_t rpc_id, const std::byte* payload,
                           int32_t len) = 0;

  // ---- Entity type registry -------------------------------------------
  // data is a serialised entity-type descriptor.
  virtual void RegisterEntityType(const std::byte* data, int32_t len) = 0;
  virtual void UnregisterAllEntityTypes() = 0;

  // data is a serialised StructDescriptor blob (see EntityDefRegistry::
  // RegisterStruct). Must be called before any RegisterEntityType that
  // references the struct by id, so the wire-side struct_id → descriptor
  // lookup is always populated in advance.
  virtual void RegisterStruct(const std::byte* data, int32_t len) = 0;

  // ---- Persistence (BaseApp / CellApp) --------------------------------
  // Trigger an async write of the entity's persistent properties to DBApp.
  // entity_data: serialised property blob produced by C# Atlas.Runtime.
  virtual void WriteToDb(uint32_t entity_id, const std::byte* entity_data, int32_t len) = 0;

  // ---- Client transfer ------------------------------------------------
  // Transfer the client connection attached to src_entity_id to
  // dest_entity_id (may live on a different BaseApp).
  virtual void GiveClientTo(uint32_t src_entity_id, uint32_t dest_entity_id) = 0;

  // ---- Script-initiated entity creation (BaseApp) --------------------
  // Create a new base entity on THIS BaseApp of the given type. Returns
  // the newly-allocated EntityID, or 0 on failure. For has_cell types
  // the call also triggers CreateCellEntity on a CellApp targeting
  // `space_id` (CellApp auto-creates the space if missing). Witness
  // enablement happens later via the client-bind path (BindClient
  // sending cellapp::EnableWitness); scripts wanting a non-default AoI
  // radius call SetAoIRadius once the cell is ready. Non-BaseApp
  // providers log an error and return 0.
  virtual auto CreateBaseEntity(uint16_t type_id, uint32_t space_id) -> uint32_t = 0;

  // ---- Runtime AoI radius adjustment (BaseApp) ------------------------
  // Forward a cellapp::SetAoIRadius to the cell that hosts this entity.
  // Clamp + Ghost rejection happen on the cell side (Witness::SetAoIRadius
  // + CellApp::OnSetAoIRadius). Providers other than BaseApp log an error
  // and no-op.
  virtual void SetAoIRadius(uint32_t entity_id, float radius, float hysteresis) = 0;

  // ---- C# → C++ callback table ----------------------------------------
  // Called by C# Atlas.Runtime once at startup to supply function pointers
  // for C++ → C# calls.  The native_callbacks blob is a packed array of
  // function pointer values whose layout is defined in clr_native_api.h.
  virtual void SetNativeCallbacks(const void* native_callbacks, int32_t len) = 0;

  // ---- CellApp spatial/replication ------------------------------------
  //
  // Only CellApp implements these non-trivially; other processes inherit
  // BaseNativeProvider's default no-op + error log. BaseApp might
  // implement SetEntityPosition in the future for the "predict own
  // client" case but for now only CellApp owns position authority.

  // C# tells us about the new world position of a cell entity. The
  // provider updates CellEntity::position_ (which triggers a RangeList
  // shuffle) and marks the entity's volatile-dirty flag so the next
  // BuildAndConsumeReplicationFrame advances VolatileSeq.
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

  // Controller registration — C# scripts create movement / timer /
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

  // ---- Client telemetry ------------------------------------------------
  // Client reports an observed reliable-delta gap to BaseApp for the
  // given entity. Only ClientNativeProvider implements it; server-side
  // providers log an error and no-op.
  virtual void ReportClientEventSeqGap(uint32_t entity_id, uint32_t gap_delta) = 0;
};

// Register the provider for this process.  Must be called before
// ClrHost::Initialize().  The pointer must remain valid until process exit.
void SetNativeApiProvider(INativeApiProvider* provider);

// Retrieve the registered provider.  Asserts (and terminates) if none was
// registered — a programming error, not a recoverable runtime failure.
INativeApiProvider& GetNativeApiProvider();

}  // namespace atlas

#endif  // ATLAS_LIB_CLRSCRIPT_NATIVE_API_PROVIDER_H_
