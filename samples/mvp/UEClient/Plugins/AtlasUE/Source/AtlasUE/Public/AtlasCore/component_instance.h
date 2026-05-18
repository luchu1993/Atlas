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

  // No sub-component nesting; entity's ApplyComponentSection feeds per-slot bytes.
  bool ApplyDelta(SpanReader& reader, AtlasEdrContext* ctx);

  // Codegen overrides with a method-idx switch.
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
