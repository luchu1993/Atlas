#ifndef ATLAS_UE_CLIENT_CORE_COMPONENT_INSTANCE_H_
#define ATLAS_UE_CLIENT_CORE_COMPONENT_INSTANCE_H_

#include <cstdint>
#include <vector>

#include "AtlasCore/core_export.h"
#include "AtlasCore/property_value.h"

struct AtlasEdrComponent;
struct AtlasEdrContext;

namespace atlas {

class ClientEntity;
class SpanReader;

// One slot on a ClientEntity. Property storage shape matches the entity body
// (flat std::vector<PropertyValue> keyed by descriptor index); codegen-
// emitted typed views read off this. Component instance also routes
// downstream RPCs for the slot — DispatchRpc is virtual so per-component
// codegen overrides for the method switch.
class ATLAS_CORE_API ComponentInstance {
 public:
  ComponentInstance(const AtlasEdrComponent* descriptor, ClientEntity* owner, uint8_t slot_idx);
  virtual ~ComponentInstance() = default;

  ComponentInstance(const ComponentInstance&) = delete;
  ComponentInstance& operator=(const ComponentInstance&) = delete;

  [[nodiscard]] const AtlasEdrComponent* Descriptor() const { return descriptor_; }
  [[nodiscard]] ClientEntity* Owner() const { return owner_; }
  [[nodiscard]] uint8_t SlotIdx() const { return slot_idx_; }
  [[nodiscard]] const std::vector<PropertyValue>& Properties() const { return properties_; }

  // Decodes one sectionMask-framed delta on this slot. Mirrors
  // ClientEntity::ApplyDelta minus the component section (components don't
  // nest). Caller (entity's ApplyComponentSection) feeds the per-slot bytes.
  bool ApplyDelta(SpanReader& reader, AtlasEdrContext* ctx);

  // Default returns false ("no handler"); codegen-emitted typed component
  // subclasses override with a method-idx switch.
  virtual bool DispatchRpc(uint32_t /*rpc_id*/, uint64_t /*trace_id*/,
                            SpanReader& /*reader*/) {
    return false;
  }

  template <typename T>
  [[nodiscard]] const T* GetScalar(std::size_t descriptor_index) const {
    if (descriptor_index >= properties_.size()) return nullptr;
    return std::get_if<T>(&properties_[descriptor_index]);
  }

 private:
  const AtlasEdrComponent* descriptor_;
  ClientEntity* owner_;
  uint8_t slot_idx_;
  std::vector<PropertyValue> properties_;
};

}  // namespace atlas

#endif  // ATLAS_UE_CLIENT_CORE_COMPONENT_INSTANCE_H_
