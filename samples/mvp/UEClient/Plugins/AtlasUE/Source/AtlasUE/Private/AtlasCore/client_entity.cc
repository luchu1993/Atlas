#include "AtlasCore/client_entity.h"

#include <cstdint>
#include <utility>

#include "AtlasCore/component_instance.h"
#include "AtlasCore/property_decoder.h"
#include "AtlasCore/span_reader.h"
#include "entitydef/entitydef_api.h"

namespace atlas {

namespace {

const AtlasEdrProperty* EntityPropAt(const void* desc, int32_t idx) {
  return AtlasEdrEntityPropertyAt(static_cast<const AtlasEdrEntity*>(desc), idx);
}

}  // namespace

void ClientEntity::BindDescriptor(const AtlasEdrEntity* descriptor, AtlasEdrContext* ctx) {
  descriptor_ = descriptor;
  edr_ctx_ = ctx;
  components_.clear();
  if (descriptor_ == nullptr) {
    properties_.clear();
    return;
  }
  const int32_t count = AtlasEdrEntityPropertyCount(descriptor_);
  // PropertyValue is move-only (unique_ptr alternatives) so assign(n, v) is
  // unavailable; resize() default-constructs each slot to monostate.
  properties_.clear();
  properties_.resize(count > 0 ? static_cast<size_t>(count) : 0u);
}

void ClientEntity::RegisterComponentFactory(uint8_t slot_idx, ComponentFactory factory) {
  if (slot_idx >= component_factories_.size()) {
    component_factories_.resize(static_cast<size_t>(slot_idx) + 1);
  }
  component_factories_[slot_idx] = std::move(factory);
}

bool ClientEntity::ApplyDelta(SpanReader& reader) {
  if (descriptor_ == nullptr) return false;
  uint8_t section_mask = 0;
  const int32_t prop_count = AtlasEdrEntityPropertyCount(descriptor_);
  if (!ApplyPropertyDelta(reader, edr_ctx_, prop_count, descriptor_, &EntityPropAt, properties_,
                          view_.get(), &section_mask)) {
    return false;
  }
  if ((section_mask & 0x04) == 0) return true;

  // Component section: [PackedUInt32 count] then per dirty slot
  //   [u8 slot_idx] [section-mask-framed component delta]
  uint32_t comp_count;
  if (!reader.ReadPackedUInt32(comp_count)) return false;
  for (uint32_t i = 0; i < comp_count; ++i) {
    uint8_t slot;
    if (!reader.Read(slot)) return false;
    const AtlasEdrComponent* comp_desc =
        AtlasEdrEntityComponentAtSlot(edr_ctx_, descriptor_, slot);
    if (comp_desc == nullptr) return false;
    if (slot >= components_.size()) components_.resize(static_cast<size_t>(slot) + 1);
    if (components_[slot] == nullptr) {
      auto factory = slot < component_factories_.size() ? component_factories_[slot] : nullptr;
      if (factory) {
        components_[slot] = factory(comp_desc, this, slot);
      } else {
        components_[slot] = std::make_unique<ComponentInstance>(comp_desc, this, slot);
      }
    }
    if (!components_[slot]->ApplyDelta(reader, edr_ctx_)) return false;
  }
  return true;
}

}  // namespace atlas
