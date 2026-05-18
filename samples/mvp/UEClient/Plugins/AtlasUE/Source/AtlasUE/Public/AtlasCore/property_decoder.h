#ifndef ATLAS_UE_CLIENT_CORE_PROPERTY_DECODER_H_
#define ATLAS_UE_CLIENT_CORE_PROPERTY_DECODER_H_

#include <cstdint>

#include "AtlasCore/property_value.h"

struct AtlasEdrContext;
struct AtlasEdrDataTypeRef;
struct AtlasEdrProperty;
struct AtlasEdrStruct;

namespace atlas {

class EntityView;
class SpanReader;

// `desc` is the caller's entity / component body opaque handle.
using PropertyAtFn = const AtlasEdrProperty* (*)(const void* desc, int32_t idx);

// Mirrors Atlas.Observable.OpKind; capped at 16 so a future 4-bit packing fits.
enum class OpKind : uint8_t {
  kSet = 0,
  kListSplice = 1,
  kDictSet = 2,
  kDictErase = 3,
  kClear = 4,
  kStructFieldSet = 5,
};

// Container wire uses u16 counts so each level caps at 65535.
[[nodiscard]] bool DecodeValue(SpanReader& reader, uint8_t data_type,
                                const AtlasEdrDataTypeRef* ref, AtlasEdrContext* ctx,
                                PropertyValue& out);

// `out` must already hold the matching ListValue / DictValue alternative.
[[nodiscard]] bool DecodeContainerOps(SpanReader& reader, const AtlasEdrDataTypeRef* ref,
                                       AtlasEdrContext* ctx, PropertyValue& out);

// `section_mask_out` returns the consumed mask so the caller can dispatch the
// component section (bit 0x04); components pass nullptr — they don't nest.
[[nodiscard]] bool ApplyPropertyDelta(SpanReader& reader, AtlasEdrContext* ctx,
                                       int32_t props_count, const void* desc,
                                       PropertyAtFn props_at,
                                       std::vector<PropertyValue>& slots,
                                       EntityView* view,
                                       uint8_t* section_mask_out);

}  // namespace atlas

#endif  // ATLAS_UE_CLIENT_CORE_PROPERTY_DECODER_H_
