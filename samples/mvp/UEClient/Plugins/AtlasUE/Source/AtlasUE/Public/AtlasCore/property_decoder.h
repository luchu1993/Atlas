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

// Adapter contract: returns the i-th property descriptor on whatever
// surface (entity body / component body) the caller's `desc` represents.
using PropertyAtFn = const AtlasEdrProperty* (*)(const void* desc, int32_t idx);

// Mirrors Atlas.Observable.OpKind. Capped at 16 so a future packed
// 4-bit-in-a-byte wire encoding still fits.
enum class OpKind : uint8_t {
  kSet = 0,
  kListSplice = 1,
  kDictSet = 2,
  kDictErase = 3,
  kClear = 4,
  kStructFieldSet = 5,
};

// Decodes one value (scalar / struct / list / dict) in full integral form
// into `out`. Containers and structs are decoded recursively, walking
// `ref` and looking up nested struct descriptors via `ctx`.
//
// Returns false on truncation, unknown data_type, missing struct
// descriptor, or oversized container (the wire uses u16 counts so the
// upper bound is hard-coded to 65535 per level).
[[nodiscard]] bool DecodeValue(SpanReader& reader, uint8_t data_type,
                                const AtlasEdrDataTypeRef* ref, AtlasEdrContext* ctx,
                                PropertyValue& out);

// Replays one container's op log against `out`, in-place. Only valid when
// `out` already holds the matching alternative (ListValue / DictValue);
// callers must seed an empty container before the first delta lands. For
// nested containers the recursion descends into child dirty slots / keys.
[[nodiscard]] bool DecodeContainerOps(SpanReader& reader, const AtlasEdrDataTypeRef* ref,
                                       AtlasEdrContext* ctx, PropertyValue& out);

// Generic sectionMask-framed delta decoder shared between entity body and
// component slots. Reads the leading sectionMask byte, applies scalar +
// container sections, fires OnPropertyChanged on `view` for each scalar /
// container that decoded, and writes the consumed sectionMask out via
// `section_mask_out` so the caller can dispatch the component section
// (bit 0x04) on entities. Caller pre-sizes `slots` to props_count
// (default-monostate) before the first call. Components have no nested
// components, so they pass nullptr for `section_mask_out`.
[[nodiscard]] bool ApplyPropertyDelta(SpanReader& reader, AtlasEdrContext* ctx,
                                       int32_t props_count, const void* desc,
                                       PropertyAtFn props_at,
                                       std::vector<PropertyValue>& slots,
                                       EntityView* view,
                                       uint8_t* section_mask_out);

}  // namespace atlas

#endif  // ATLAS_UE_CLIENT_CORE_PROPERTY_DECODER_H_
