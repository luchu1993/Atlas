#ifndef ATLAS_UE_CLIENT_CORE_CLIENT_ENTITY_H_
#define ATLAS_UE_CLIENT_CORE_CLIENT_ENTITY_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "AtlasCore/component_instance.h"
#include "AtlasCore/core_export.h"
#include "AtlasCore/entity_id.h"
#include "AtlasCore/entity_view.h"
#include "AtlasCore/property_value.h"

struct AtlasEdrEntity;
struct AtlasEdrContext;
struct AtlasMovementStateFrame;

namespace atlas {

class RpcSender;
class SpanReader;

struct MovementCommandFrame {
  uint32_t command_id{0};
  uint16_t skill_id{0};
  uint8_t type{0};
  Vec3 start_position{};
  Vec3 target_position{};
  uint16_t duration_ms{0};
  uint16_t elapsed_ms{0};
  uint16_t curve_id{0};
  uint8_t input_policy{0};
  uint8_t collision_policy{0};
  uint8_t priority{0};
  uint32_t server_tick{0};
};

enum class MovementCommandEndReason : uint8_t {
  kCompleted = 0,
  kCancelled = 1,
  kCollision = 2,
  kInvalid = 3,
};

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

  // Default no-op so non-spatial entities don't pay AvatarFilter cost.
  virtual void OnPositionReceived(double /*server_time*/, const Vec3& /*pos*/,
                                  const Vec3& /*dir*/, bool /*on_ground*/) {}

  virtual void OnMovementStateAck(uint32_t /*acked_input_seq*/, uint32_t /*server_tick*/,
                                  const AtlasMovementStateFrame& /*state*/,
                                  uint16_t /*correction_flags*/) {}

  virtual void OnMovementCommandStart(const MovementCommandFrame& /*command*/) {}

  virtual void OnMovementCommandEnd(uint32_t /*command_id*/, uint32_t /*server_tick*/,
                                    MovementCommandEndReason /*reason*/,
                                    const AtlasMovementStateFrame& /*state*/) {}

  virtual void TickInterpolation(double /*dt*/) {}

  // ApplyDelta returns false until a descriptor is bound.
  void BindDescriptor(const AtlasEdrEntity* descriptor, AtlasEdrContext* ctx);

  [[nodiscard]] const AtlasEdrEntity* Descriptor() const { return descriptor_; }

  [[nodiscard]] const std::vector<PropertyValue>& Properties() const { return properties_; }

  // Null sender means RPC sends drop silently (tests that only exercise decode).
  void SetRpcSender(RpcSender* sender) { sender_ = sender; }
  [[nodiscard]] RpcSender* Sender() const { return sender_; }

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

  // Slot 0 is the entity body; component slots lazily allocate on first delta.
  [[nodiscard]] ComponentInstance* GetComponent(uint8_t slot_idx) const {
    if (slot_idx >= components_.size()) return nullptr;
    return components_[slot_idx].get();
  }

  // Without a registered factory the slot decodes into a base ComponentInstance.
  using ComponentFactory = std::function<std::unique_ptr<ComponentInstance>(
      const AtlasEdrComponent*, ClientEntity*, uint8_t)>;
  void RegisterComponentFactory(uint8_t slot_idx, ComponentFactory factory);

  // Virtual so codegen subclasses can snapshot scalars and diff + fire
  // On<Prop>Changed after the base decode.
  virtual bool ApplyDelta(SpanReader& reader);

  // Codegen overrides DispatchEntityRpc, not this dispatch entry.
  bool DispatchRpc(uint32_t rpc_id, uint64_t trace_id, SpanReader& reader) {
    const uint8_t slot = (rpc_id >> 24) & 0x7F;
    if (slot == 0) return DispatchEntityRpc(rpc_id, trace_id, reader);
    ComponentInstance* c = GetComponent(slot);
    return c != nullptr && c->DispatchRpc(rpc_id, trace_id, reader);
  }

 protected:
  virtual bool DispatchEntityRpc(uint32_t /*rpc_id*/, uint64_t /*trace_id*/,
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
  std::vector<std::unique_ptr<ComponentInstance>> components_;
  std::vector<ComponentFactory> component_factories_;
};

}  // namespace atlas

#endif  // ATLAS_UE_CLIENT_CORE_CLIENT_ENTITY_H_
