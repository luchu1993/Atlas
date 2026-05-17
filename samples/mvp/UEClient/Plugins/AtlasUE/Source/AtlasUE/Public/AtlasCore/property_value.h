#ifndef ATLAS_UE_CLIENT_CORE_PROPERTY_VALUE_H_
#define ATLAS_UE_CLIENT_CORE_PROPERTY_VALUE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "AtlasCore/entity_view.h"

namespace atlas {

// Forward declarations so PropertyValue can name them; full definitions
// follow below so the variant's destructor sees complete types in every
// TU that includes this header.
struct ListValue;
struct DictValue;
struct StructValue;

// PropertyValue is move-only because the container alternatives wrap their
// payload in unique_ptr to break the otherwise-circular type definition.
// std::vector tolerates move-only element types, so storage stays in a
// flat std::vector<PropertyValue> at every nesting level.
using PropertyValue = std::variant<
    std::monostate, bool,
    int8_t, uint8_t, int16_t, uint16_t,
    int32_t, uint32_t, int64_t, uint64_t,
    float, double,
    std::string, std::vector<uint8_t>,
    Vec3, Quat,
    std::unique_ptr<ListValue>,
    std::unique_ptr<DictValue>,
    std::unique_ptr<StructValue>>;

struct ListValue {
  std::vector<PropertyValue> items;
};

// Dict keys are restricted to scalar kinds by the .def parser (string + fixed-
// width ints), so a flat vector + linear scan beats any hashed map for the
// sizes we see in practice (≤ low hundreds of entries per dict property).
struct DictValue {
  std::vector<std::pair<PropertyValue, PropertyValue>> entries;
};

// `struct_id` is the descriptor-side struct handle; kept on the value so
// debug tooling and asserts can validate that the wire decode matched the
// descriptor the property was bound against.
struct StructValue {
  uint16_t struct_id{0};
  std::vector<PropertyValue> fields;
};

}  // namespace atlas

#endif  // ATLAS_UE_CLIENT_CORE_PROPERTY_VALUE_H_
