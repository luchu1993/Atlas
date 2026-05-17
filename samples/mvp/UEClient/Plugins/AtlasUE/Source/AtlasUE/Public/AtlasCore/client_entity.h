#ifndef ATLAS_UE_CLIENT_CORE_CLIENT_ENTITY_H_
#define ATLAS_UE_CLIENT_CORE_CLIENT_ENTITY_H_

#include <memory>
#include <vector>

#include "AtlasCore/core_export.h"
#include "AtlasCore/entity_id.h"
#include "AtlasCore/entity_view.h"
#include "AtlasCore/property_value.h"

struct AtlasEdrEntity;
struct AtlasEdrContext;

namespace atlas {

class RpcSender;
class SpanReader;

class ATLAS_CORE_API ClientEntity {
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

  // Hooks for derived typed entities. Default no-op so entities that do not
  // observe transforms (e.g. inventory-only ghosts) carry no AvatarFilter cost.
  virtual void OnPositionReceived(double /*server_time*/, const Vec3& /*pos*/,
                                  const Vec3& /*dir*/, bool /*on_ground*/) {}

  virtual void TickInterpolation(double /*dt*/) {}

  // Binds the per-type descriptor used by ApplyDelta + the owning registry
  // context (needed to resolve nested struct ids inside container values).
  // Entities created before bind carry no property storage; ApplyDelta
  // returns false until a descriptor is bound.
  void BindDescriptor(const AtlasEdrEntity* descriptor, AtlasEdrContext* ctx);

  [[nodiscard]] const AtlasEdrEntity* Descriptor() const { return descriptor_; }

  [[nodiscard]] const std::vector<PropertyValue>& Properties() const { return properties_; }

  // Outbound RPC bridge. Codegen-emitted upstream stubs (entity->ReportPos
  // etc.) read this to deliver wire bytes; null means RPC sends drop on
  // the floor (legitimate in unit tests that only exercise decode).
  void SetRpcSender(RpcSender* sender) { sender_ = sender; }
  [[nodiscard]] RpcSender* Sender() const { return sender_; }

  // Typed scalar accessor used by codegen-emitted getters. Returns nullptr
  // for missing / wrong-type slots; codegen wraps with a fallback default
  // for ergonomic property access.
  template <typename T>
  [[nodiscard]] const T* GetScalar(std::size_t descriptor_index) const {
    if (descriptor_index >= properties_.size()) return nullptr;
    return std::get_if<T>(&properties_[descriptor_index]);
  }

  [[nodiscard]] const ListValue* GetList(std::size_t descriptor_index) const {
    if (descriptor_index >= properties_.size()) return nullptr;
    const auto* slot = std::get_if<std::unique_ptr<ListValue>>(&properties_[descriptor_index]);
    return slot != nullptr ? slot->get() : nullptr;
  }

  [[nodiscard]] const DictValue* GetDict(std::size_t descriptor_index) const {
    if (descriptor_index >= properties_.size()) return nullptr;
    const auto* slot = std::get_if<std::unique_ptr<DictValue>>(&properties_[descriptor_index]);
    return slot != nullptr ? slot->get() : nullptr;
  }

  [[nodiscard]] const StructValue* GetStruct(std::size_t descriptor_index) const {
    if (descriptor_index >= properties_.size()) return nullptr;
    const auto* slot = std::get_if<std::unique_ptr<StructValue>>(&properties_[descriptor_index]);
    return slot != nullptr ? slot->get() : nullptr;
  }

  // Decodes one sectionMask-framed delta — scalars, containers, and structs.
  // Returns false on truncation or malformed wire (out-of-range slot, missing
  // descriptor, unknown op kind); storage is left in whatever partial state
  // the decode reached so the caller can drop the entity.
  // Virtual so codegen subclasses can snapshot prior scalar values, run the
  // base decode, then diff + fire On<Prop>Changed hooks.
  virtual bool ApplyDelta(SpanReader& reader);

  // Inbound 0xF004 RPC entry point. Default returns false ("no handler");
  // codegen-emitted typed entities override and switch on rpc_id to decode
  // args + invoke the per-method virtual.
  virtual bool DispatchRpc(uint32_t /*rpc_id*/, uint64_t /*trace_id*/,
                            SpanReader& /*reader*/) {
    return false;
  }

 private:
  EntityId id_;
  EntityTypeId type_id_;
  std::unique_ptr<EntityView> view_;
  const AtlasEdrEntity* descriptor_{nullptr};
  AtlasEdrContext* edr_ctx_{nullptr};
  RpcSender* sender_{nullptr};
  std::vector<PropertyValue> properties_;
};

}  // namespace atlas

#endif  // ATLAS_UE_CLIENT_CORE_CLIENT_ENTITY_H_
