#ifndef ATLAS_UE_CLIENT_CORE_CLIENT_ENTITY_H_
#define ATLAS_UE_CLIENT_CORE_CLIENT_ENTITY_H_

#include <memory>

#include "AtlasCore/entity_id.h"
#include "AtlasCore/entity_view.h"

namespace atlas {

class ClientEntity {
 public:
  ClientEntity(EntityId id, EntityTypeId type_id) : id_(id), type_id_(type_id) {}
  virtual ~ClientEntity() = default;

  ClientEntity(const ClientEntity&) = delete;
  ClientEntity& operator=(const ClientEntity&) = delete;

  [[nodiscard]] EntityId Id() const { return id_; }
  [[nodiscard]] EntityTypeId TypeId() const { return type_id_; }

  void AttachView(std::unique_ptr<EntityView> view) { view_ = std::move(view); }
  void DetachView() { view_.reset(); }
  [[nodiscard]] EntityView* View() const { return view_.get(); }

 private:
  EntityId id_;
  EntityTypeId type_id_;
  std::unique_ptr<EntityView> view_;
};

}  // namespace atlas

#endif  // ATLAS_UE_CLIENT_CORE_CLIENT_ENTITY_H_
