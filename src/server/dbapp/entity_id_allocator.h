#ifndef ATLAS_SERVER_DBAPP_ENTITY_ID_ALLOCATOR_H_
#define ATLAS_SERVER_DBAPP_ENTITY_ID_ALLOCATOR_H_

#include <cstdint>
#include <functional>

#include "db/idatabase.h"
#include "foundation/log.h"
#include "server/entity_types.h"

namespace atlas {

// Persists a forward-only watermark so crash recovery never reuses handed-out IDs.
class EntityIdAllocator {
 public:
  explicit EntityIdAllocator(IDatabase& db);

  void Startup(std::function<void(bool)> callback);

  [[nodiscard]] auto Allocate(uint32_t count) -> std::pair<EntityID, EntityID>;

  void PersistIfNeeded(std::function<void(bool)> callback);

  void Persist(std::function<void(bool)> callback);

  [[nodiscard]] auto next_id() const -> EntityID { return next_id_; }

 private:
  IDatabase& db_;
  EntityID next_id_{1};
  EntityID persisted_up_to_{0};

  static constexpr EntityID kSafetyBuffer = 100'000;
};

}  // namespace atlas

#endif  // ATLAS_SERVER_DBAPP_ENTITY_ID_ALLOCATOR_H_
