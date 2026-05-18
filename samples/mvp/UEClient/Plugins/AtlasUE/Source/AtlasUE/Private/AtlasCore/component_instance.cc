#include "AtlasCore/component_instance.h"

#include "AtlasCore/property_decoder.h"
#include "AtlasCore/span_reader.h"
#include "entitydef/entitydef_api.h"

namespace atlas {

namespace {

const AtlasEdrProperty* ComponentPropAt(const void* desc, int32_t idx) {
  return AtlasEdrComponentPropertyAt(static_cast<const AtlasEdrComponent*>(desc), idx);
}

}  // namespace

ComponentInstance::ComponentInstance(const AtlasEdrComponent* descriptor, ClientEntity* owner,
                                      uint8_t slot_idx)
    : descriptor_(descriptor), owner_(owner), slot_idx_(slot_idx) {
  if (descriptor_ != nullptr) {
    const int32_t count = AtlasEdrComponentPropertyCount(descriptor_);
    properties_.resize(count > 0 ? static_cast<size_t>(count) : 0u);
  }
}

bool ComponentInstance::ApplyDelta(SpanReader& reader, AtlasEdrContext* ctx) {
  if (descriptor_ == nullptr) return false;
  const int32_t count = AtlasEdrComponentPropertyCount(descriptor_);
  return ApplyPropertyDelta(reader, ctx, count, descriptor_, &ComponentPropAt, properties_,
                            /*view=*/nullptr, /*section_mask_out=*/nullptr);
}

}  // namespace atlas
