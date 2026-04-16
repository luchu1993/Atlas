#ifndef ATLAS_LIB_DB_XML_XML_DATABASE_H_
#define ATLAS_LIB_DB_XML_XML_DATABASE_H_

#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "db/idatabase.h"
#include "foundation/clock.h"

namespace atlas {

// ============================================================================
// XmlDatabase — file-based development/test database backend
//
// Storage layout:
//   <xml_dir>/meta.json          — { "next_dbid": N }
//   <xml_dir>/<TypeName>/        — one subdirectory per entity type
//   <xml_dir>/<TypeName>/<id>.bin — entity blob (binary)
//   <xml_dir>/<TypeName>/index.json — { "<identifier>": <dbid> }
//   <xml_dir>/auto_load.json     — [{ "type_id": N, "dbid": N }, ...]
//
// Callback execution mode:
//   - Default (deferred_mode_=false): callbacks are invoked synchronously
//     inside each API call, before it returns.
//   - Deferred (deferred_mode_=true): callbacks are queued and only invoked
//     when process_results() is called.  Use this mode in tests to simulate
//     the asynchronous behavior of the MySQL backend.
//
// Flush policy:
//   - Buffered (default): blob writes are staged in memory and flushed later
//     to keep the XML backend cheap during development. Successful callbacks
//     mean "visible in this process", not "durable on disk".
//   - Immediate: each mutating call flushes staged state before callbacks are
//     observed. Use this for tests that need durable-on-return semantics.
// ============================================================================

class XmlDatabase : public IDatabase {
 public:
  enum class FlushPolicy {
    kBuffered,
    kImmediate,
  };

  XmlDatabase() = default;
  ~XmlDatabase() override;

  void SetDeferredMode(bool enabled) override { deferred_mode_ = enabled; }
  void SetFlushPolicy(FlushPolicy policy);

  [[nodiscard]] auto Startup(const DatabaseConfig& config, const EntityDefRegistry& entity_defs)
      -> Result<void> override;
  void Shutdown() override;

  void PutEntity(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                 std::span<const std::byte> blob, const std::string& identifier,
                 std::function<void(PutResult)> callback) override;

  void PutEntityWithPassword(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                             std::span<const std::byte> blob, const std::string& identifier,
                             const std::string& password_hash,
                             std::function<void(PutResult)> callback) override;

  void GetEntity(DatabaseID dbid, uint16_t type_id,
                 std::function<void(GetResult)> callback) override;

  void DelEntity(DatabaseID dbid, uint16_t type_id,
                 std::function<void(DelResult)> callback) override;

  void LookupByName(uint16_t type_id, const std::string& identifier,
                    std::function<void(LookupResult)> callback) override;

  void CheckoutEntity(DatabaseID dbid, uint16_t type_id, const CheckoutInfo& new_owner,
                      std::function<void(GetResult)> callback) override;

  void CheckoutEntityByName(uint16_t type_id, const std::string& identifier,
                            const CheckoutInfo& new_owner,
                            std::function<void(GetResult)> callback) override;

  void ClearCheckout(DatabaseID dbid, uint16_t type_id,
                     std::function<void(bool)> callback) override;

  void ClearCheckoutsForAddress(const Address& base_addr,
                                std::function<void(int cleared_count)> callback) override;

  void MarkCheckoutCleared(DatabaseID dbid, uint16_t type_id) override;

  void GetAutoLoadEntities(std::function<void(std::vector<EntityData>)> callback) override;

  void SetAutoLoad(DatabaseID dbid, uint16_t type_id, bool auto_load) override;

  void LoadEntityIdCounter(std::function<void(EntityID next_id)> callback) override;
  void SaveEntityIdCounter(EntityID next_id, std::function<void(bool success)> callback) override;

  void ProcessResults() override;

 private:
  // ---- Storage helpers ----
  [[nodiscard]] auto TypeDir(uint16_t type_id) const -> std::filesystem::path;
  [[nodiscard]] auto TypeName(uint16_t type_id) const -> std::string;
  [[nodiscard]] auto BlobPath(uint16_t type_id, DatabaseID dbid) const -> std::filesystem::path;

  void LoadMeta();
  void SaveMeta();
  void LoadIndex(uint16_t type_id);
  void SaveIndex(uint16_t type_id);
  void LoadAutoLoad();
  void SaveAutoLoad();
  void LoadCheckouts();
  void SaveCheckouts();
  void LoadPasswordHashes();
  void SavePasswordHashes();
  void MarkMetaDirty();
  void MarkIndexDirty(uint16_t type_id);
  void MarkAutoLoadDirty();
  void MarkCheckoutsDirty();
  void MarkPasswordHashesDirty();
  void FlushAfterMutation();
  void FlushDirtyState(bool force = false);
  void StageBlobWrite(uint16_t type_id, DatabaseID dbid, std::span<const std::byte> data);
  void StageBlobDelete(uint16_t type_id, DatabaseID dbid);
  void FlushPendingBlobWrites(int budget);

  [[nodiscard]] auto ReadBlob(uint16_t type_id, DatabaseID dbid) const
      -> std::optional<std::vector<std::byte>>;
  void WriteBlob(uint16_t type_id, DatabaseID dbid, std::span<const std::byte> data);
  void DeleteBlob(uint16_t type_id, DatabaseID dbid);

  // ---- Deferred callback queue ----
  void FireOrDefer(std::function<void()> cb);

  // ---- State ----
  std::filesystem::path base_dir_;
  const EntityDefRegistry* entity_defs_{nullptr};
  DatabaseID next_dbid_{1};
  bool started_{false};
  bool deferred_mode_{false};
  FlushPolicy flush_policy_{FlushPolicy::kBuffered};
  TimePoint next_flush_deadline_{};
  TimePoint next_metadata_flush_deadline_{};
  bool meta_dirty_{false};
  bool auto_load_dirty_{false};
  bool checkouts_dirty_{false};
  bool password_hashes_dirty_{false};

  // name → DBID index (per type_id)
  std::unordered_map<uint16_t, std::unordered_map<std::string, DatabaseID>> name_index_;
  std::unordered_set<uint16_t> dirty_indexes_;

  // checkout: key = (type_id << 48) | (dbid & 0xFFFFFFFFFFFF)
  std::unordered_map<uint64_t, CheckoutInfo> checkouts_;
  std::unordered_map<uint64_t, std::string> password_hashes_;
  struct PendingBlobWrite {
    uint16_t type_id{0};
    DatabaseID dbid{kInvalidDBID};
    std::vector<std::byte> data;
    bool deleted{false};
  };
  std::unordered_map<uint64_t, PendingBlobWrite> pending_blob_writes_;

  // auto-load set: same key encoding
  std::set<uint64_t> auto_load_set_;

  // type_id → type name (from EntityDefRegistry)
  std::unordered_map<uint16_t, std::string> type_names_;

  // deferred callbacks
  std::deque<std::function<void()>> deferred_;

  // blob read cache — populated on flush and disk reads, avoids repeated I/O
  mutable std::unordered_map<uint64_t, std::vector<std::byte>> blob_cache_;
  size_t pending_blob_bytes_{0};

  static constexpr auto CheckoutKey(uint16_t type_id, DatabaseID dbid) -> uint64_t {
    return (static_cast<uint64_t>(type_id) << 48) |
           (static_cast<uint64_t>(dbid) & 0x0000FFFFFFFFFFFFULL);
  }

  static constexpr Duration kFlushInterval = std::chrono::milliseconds(200);
  static constexpr Duration kMetadataFlushInterval = std::chrono::seconds(2);
  static constexpr int kMaxCallbacksPerTick = 2048;
  static constexpr int kMaxBlobWritesPerFlush = 16;
  static constexpr size_t kMaxPendingBlobWrites = 64;
  static constexpr size_t kMaxPendingBlobBytes = 4 * 1024 * 1024;
};

}  // namespace atlas

#endif  // ATLAS_LIB_DB_XML_XML_DATABASE_H_
