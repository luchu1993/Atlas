#ifndef ATLAS_LIB_ENTITYDEF_STRUCT_DESCRIPTOR_H_
#define ATLAS_LIB_ENTITYDEF_STRUCT_DESCRIPTOR_H_

#include <cstdint>
#include <string>
#include <vector>

#include "entitydef/entity_type_descriptor.h"

namespace atlas {

// Field index in `fields` is the stable wire-side id — field-level ops
// reference a field by its position, not its name. Don't reorder fields
// without bumping a schema version.
struct FieldDescriptor {
  std::string name;
  DataTypeRef type;
};

// `id` is the compact handle used on the wire; must be unique within a
// process. Cycle detection is intentionally NOT done here — the caller is
// responsible for topological validation before RegisterStruct.
struct StructDescriptor {
  uint16_t id{0};
  std::string name;
  std::vector<FieldDescriptor> fields;
};

}  // namespace atlas

#endif  // ATLAS_LIB_ENTITYDEF_STRUCT_DESCRIPTOR_H_
