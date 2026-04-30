// Wire-format round-trip for the container-property tail in RegisterType.
//
// Container properties emit `[DataTypeRef body] [PackedInt max_size]` with
// NO leading kind byte on the body — prop.data_type already pins the
// top-level kind. Nested refs INSIDE the body (list.elem, dict.key/value)
// do carry their own kind byte because the decoder only knows their kind
// after reading it.

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "entitydef/entity_def_registry.h"
#include "serialization/binary_stream.h"

namespace atlas {

namespace {

// ---- DataTypeRef writers.
//
// Two flavours:
//   "Full" writers (WriteScalarType / WriteListType / WriteDictType /
//   WriteStructType) emit a complete DataTypeRef with leading kind byte.
//   Used for NESTED children (list.elem, dict.key, dict.value) — the recursive
//   decoder needs the kind byte there.
//
//   "Body" writers (WriteListBody / WriteDictBody / WriteStructBody) emit
//   only the kind-specific tail, no leading kind byte. Used for the
//   top-level property tail in RegisterType where prop.data_type doubles
//   as the top-level kind and the wire skips the redundant byte.

void WriteScalarType(BinaryWriter& w, PropertyDataType kind) {
  w.Write<uint8_t>(static_cast<uint8_t>(kind));
}

void WriteListType(BinaryWriter& w, const std::function<void(BinaryWriter&)>& write_elem) {
  w.Write<uint8_t>(static_cast<uint8_t>(PropertyDataType::kList));
  write_elem(w);
}

void WriteDictType(BinaryWriter& w, const std::function<void(BinaryWriter&)>& write_key,
                   const std::function<void(BinaryWriter&)>& write_value) {
  w.Write<uint8_t>(static_cast<uint8_t>(PropertyDataType::kDict));
  write_key(w);
  write_value(w);
}

void WriteStructType(BinaryWriter& w, uint16_t struct_id) {
  w.Write<uint8_t>(static_cast<uint8_t>(PropertyDataType::kStruct));
  w.Write<uint16_t>(struct_id);
}

void WriteListBody(BinaryWriter& w, const std::function<void(BinaryWriter&)>& write_elem) {
  write_elem(w);  // no top-level kind byte
}

void WriteDictBody(BinaryWriter& w, const std::function<void(BinaryWriter&)>& write_key,
                   const std::function<void(BinaryWriter&)>& write_value) {
  write_key(w);
  write_value(w);
}

void WriteStructBody(BinaryWriter& w, uint16_t struct_id) {
  w.Write<uint16_t>(struct_id);
}

// Mirrors the RegisterType wire layout emitted by TypeRegistryEmitter.
// Container properties carry the [type_ref body][max_size] tail.
struct PropSpec {
  std::string_view name;
  PropertyDataType data_type;
  ReplicationScope scope = ReplicationScope::kAllClients;
  bool persistent = false;
  bool identifier = false;
  bool reliable = false;
  uint16_t index = 0;
  // Only populated for container kinds; writer emits the DataTypeRef body.
  std::function<void(BinaryWriter&)> write_type_ref;
  // Only honoured for container kinds. Default = 4096 matches the struct
  // default so "don't care" tests stay clean.
  uint32_t max_size = 4096;
};

auto MakeTypeBlob(std::string_view type_name, uint16_t type_id,
                  std::initializer_list<PropSpec> props) -> std::vector<std::byte> {
  BinaryWriter w;
  w.WriteString(type_name);
  w.Write<uint16_t>(type_id);
  w.Write<uint8_t>(1);  // has_cell
  w.Write<uint8_t>(1);  // has_client
  w.WritePackedInt(static_cast<uint32_t>(props.size()));
  for (const auto& p : props) {
    w.WriteString(p.name);
    w.Write<uint8_t>(static_cast<uint8_t>(p.data_type));
    w.Write<uint8_t>(static_cast<uint8_t>(p.scope));
    w.Write<uint8_t>(p.persistent ? 1 : 0);
    w.Write<uint8_t>(5);  // detail_level
    w.Write<uint16_t>(p.index);
    w.Write<uint8_t>(p.identifier ? 1 : 0);
    w.Write<uint8_t>(p.reliable ? 1 : 0);

    const bool is_container = p.data_type == PropertyDataType::kList ||
                              p.data_type == PropertyDataType::kDict ||
                              p.data_type == PropertyDataType::kStruct;
    if (is_container) {
      p.write_type_ref(w);
      w.WritePackedInt(p.max_size);
    }
  }
  w.WritePackedInt(0);  // 0 rpcs
  return w.Detach();
}

class RegistryContainerPropTest : public ::testing::Test {
 protected:
  void SetUp() override { EntityDefRegistry::Instance().clear(); }
  void TearDown() override { EntityDefRegistry::Instance().clear(); }
};

}  // namespace

TEST_F(RegistryContainerPropTest, ScalarOnlyTypeStillWorks) {
  // Scalar-only blobs must not accidentally trigger the container-tail read
  // path. type_ref stays empty, max_size stays at its default.
  auto blob = MakeTypeBlob("Hello", 1,
                           {
                               PropSpec{.name = "hp", .data_type = PropertyDataType::kInt32},
                               PropSpec{.name = "name", .data_type = PropertyDataType::kString},
                           });
  ASSERT_TRUE(
      EntityDefRegistry::Instance().RegisterType(blob.data(), static_cast<int32_t>(blob.size())));
  const auto* d = EntityDefRegistry::Instance().FindByName("Hello");
  ASSERT_NE(d, nullptr);
  ASSERT_EQ(d->properties.size(), 2u);
  EXPECT_EQ(d->properties[0].data_type, PropertyDataType::kInt32);
  EXPECT_FALSE(d->properties[0].type_ref.has_value());
  EXPECT_EQ(d->properties[0].max_size, 4096u);
  EXPECT_EQ(d->properties[1].data_type, PropertyDataType::kString);
  EXPECT_FALSE(d->properties[1].type_ref.has_value());
}

TEST_F(RegistryContainerPropTest, ListPropertyDecoded) {
  // list[int32]: body = one nested DataTypeRef (the elem), which is a
  // scalar and thus just its kind byte.
  auto blob = MakeTypeBlob("Avatar", 10,
                           {
                               PropSpec{.name = "titles",
                                        .data_type = PropertyDataType::kList,
                                        .write_type_ref =
                                            [](BinaryWriter& w) {
                                              WriteListBody(w, [](BinaryWriter& ww) {
                                                WriteScalarType(ww, PropertyDataType::kInt32);
                                              });
                                            },
                                        .max_size = 128},
                           });
  ASSERT_TRUE(
      EntityDefRegistry::Instance().RegisterType(blob.data(), static_cast<int32_t>(blob.size())));
  const auto* d = EntityDefRegistry::Instance().FindByName("Avatar");
  ASSERT_NE(d, nullptr);
  ASSERT_EQ(d->properties.size(), 1u);
  const auto& p = d->properties[0];
  EXPECT_EQ(p.name, "titles");
  EXPECT_EQ(p.data_type, PropertyDataType::kList);
  EXPECT_EQ(p.max_size, 128u);
  ASSERT_TRUE(p.type_ref.has_value());
  EXPECT_EQ(p.type_ref->kind, PropertyDataType::kList);
  ASSERT_NE(p.type_ref->elem, nullptr);
  EXPECT_EQ(p.type_ref->elem->kind, PropertyDataType::kInt32);
}

TEST_F(RegistryContainerPropTest, DictPropertyDecoded) {
  // dict[string,int32]: body = two nested DataTypeRefs (key, then value).
  auto blob = MakeTypeBlob(
      "Avatar", 11,
      {
          PropSpec{
              .name = "counters",
              .data_type = PropertyDataType::kDict,
              .write_type_ref =
                  [](BinaryWriter& w) {
                    WriteDictBody(
                        w, [](BinaryWriter& ww) { WriteScalarType(ww, PropertyDataType::kString); },
                        [](BinaryWriter& ww) { WriteScalarType(ww, PropertyDataType::kInt32); });
                  },
              .max_size = 64},
      });
  ASSERT_TRUE(
      EntityDefRegistry::Instance().RegisterType(blob.data(), static_cast<int32_t>(blob.size())));
  const auto* d = EntityDefRegistry::Instance().FindByName("Avatar");
  ASSERT_NE(d, nullptr);
  ASSERT_EQ(d->properties.size(), 1u);
  const auto& p = d->properties[0];
  EXPECT_EQ(p.max_size, 64u);
  ASSERT_TRUE(p.type_ref.has_value());
  EXPECT_EQ(p.type_ref->kind, PropertyDataType::kDict);
  ASSERT_NE(p.type_ref->key, nullptr);
  ASSERT_NE(p.type_ref->elem, nullptr);
  EXPECT_EQ(p.type_ref->key->kind, PropertyDataType::kString);
  EXPECT_EQ(p.type_ref->elem->kind, PropertyDataType::kInt32);
}

TEST_F(RegistryContainerPropTest, StructPropertyDecoded) {
  auto blob =
      MakeTypeBlob("Avatar", 12,
                   {
                       PropSpec{.name = "weapon",
                                .data_type = PropertyDataType::kStruct,
                                .write_type_ref = [](BinaryWriter& w) { WriteStructBody(w, 77); },
                                .max_size = 4096},
                   });
  ASSERT_TRUE(
      EntityDefRegistry::Instance().RegisterType(blob.data(), static_cast<int32_t>(blob.size())));
  const auto* d = EntityDefRegistry::Instance().FindByName("Avatar");
  ASSERT_NE(d, nullptr);
  const auto& p = d->properties[0];
  EXPECT_EQ(p.data_type, PropertyDataType::kStruct);
  ASSERT_TRUE(p.type_ref.has_value());
  EXPECT_EQ(p.type_ref->kind, PropertyDataType::kStruct);
  EXPECT_EQ(p.type_ref->struct_id, 77);
}

TEST_F(RegistryContainerPropTest, MixedScalarAndContainerProperties) {
  auto blob =
      MakeTypeBlob("Avatar", 13,
                   {
                       PropSpec{.name = "hp", .data_type = PropertyDataType::kInt32, .index = 0},
                       PropSpec{.name = "bag",
                                .data_type = PropertyDataType::kList,
                                .index = 1,
                                .write_type_ref =
                                    [](BinaryWriter& w) {
                                      WriteListBody(w, [](BinaryWriter& ww) {
                                        WriteScalarType(ww, PropertyDataType::kInt32);
                                      });
                                    },
                                .max_size = 256},
                       PropSpec{.name = "level", .data_type = PropertyDataType::kInt32, .index = 2},
                   });
  ASSERT_TRUE(
      EntityDefRegistry::Instance().RegisterType(blob.data(), static_cast<int32_t>(blob.size())));
  const auto* d = EntityDefRegistry::Instance().FindByName("Avatar");
  ASSERT_NE(d, nullptr);
  ASSERT_EQ(d->properties.size(), 3u);
  EXPECT_FALSE(d->properties[0].type_ref.has_value());
  EXPECT_TRUE(d->properties[1].type_ref.has_value());
  EXPECT_EQ(d->properties[1].max_size, 256u);
  EXPECT_FALSE(d->properties[2].type_ref.has_value());
}

TEST_F(RegistryContainerPropTest, InvariantRejectsTruncatedContainerTail) {
  // data_type=kList but the tail is missing — decoder must fail cleanly
  // instead of consuming downstream bytes and reporting cascade failures.
  BinaryWriter w;
  w.WriteString("Truncated");
  w.Write<uint16_t>(30);
  w.Write<uint8_t>(1);  // has_cell
  w.Write<uint8_t>(1);  // has_client
  w.WritePackedInt(1);  // 1 property
  w.WriteString("bag");
  w.Write<uint8_t>(static_cast<uint8_t>(PropertyDataType::kList));
  w.Write<uint8_t>(static_cast<uint8_t>(ReplicationScope::kAllClients));
  w.Write<uint8_t>(0);  // persistent
  w.Write<uint8_t>(5);  // detail_level
  w.Write<uint16_t>(0);
  w.Write<uint8_t>(0);  // identifier
  w.Write<uint8_t>(0);  // reliable
  // Intentionally omit type_ref body and max_size.
  auto blob = w.Detach();
  EXPECT_FALSE(
      EntityDefRegistry::Instance().RegisterType(blob.data(), static_cast<int32_t>(blob.size())))
      << "Missing container tail must fail the read.";
}

TEST_F(RegistryContainerPropTest, NestedListOfListProperty) {
  // Exercises the property-tail path on a deep type. The OUTER list is the
  // tail body (no leading kind), its elem is a full nested DataTypeRef —
  // list[int32] — with its own kind byte.
  auto blob = MakeTypeBlob("Grid", 40,
                           {
                               PropSpec{.name = "rows",
                                        .data_type = PropertyDataType::kList,
                                        .write_type_ref =
                                            [](BinaryWriter& w) {
                                              WriteListBody(w, [](BinaryWriter& ww) {
                                                WriteListType(ww, [](BinaryWriter& www) {
                                                  WriteScalarType(www, PropertyDataType::kInt32);
                                                });
                                              });
                                            }},
                           });
  ASSERT_TRUE(
      EntityDefRegistry::Instance().RegisterType(blob.data(), static_cast<int32_t>(blob.size())));
  const auto* d = EntityDefRegistry::Instance().FindByName("Grid");
  ASSERT_NE(d, nullptr);
  const auto& p = d->properties[0];
  ASSERT_TRUE(p.type_ref.has_value());
  EXPECT_EQ(p.type_ref->kind, PropertyDataType::kList);
  ASSERT_NE(p.type_ref->elem, nullptr);
  EXPECT_EQ(p.type_ref->elem->kind, PropertyDataType::kList);
  ASSERT_NE(p.type_ref->elem->elem, nullptr);
  EXPECT_EQ(p.type_ref->elem->elem->kind, PropertyDataType::kInt32);
}

TEST_F(RegistryContainerPropTest, ValidatePropertyInvariantDirectly) {
  // Covers the helper with hand-built PropertyDescriptors — faster than
  // round-tripping a blob for every invariant arm.
  PropertyDescriptor scalar_ok;
  scalar_ok.data_type = PropertyDataType::kInt32;
  EXPECT_TRUE(ValidatePropertyInvariant(scalar_ok));

  PropertyDescriptor scalar_bad_has_tail;
  scalar_bad_has_tail.data_type = PropertyDataType::kInt32;
  scalar_bad_has_tail.type_ref.emplace();
  scalar_bad_has_tail.type_ref->kind = PropertyDataType::kInt32;
  EXPECT_FALSE(ValidatePropertyInvariant(scalar_bad_has_tail))
      << "Scalar properties must not carry a DataTypeRef tail.";

  PropertyDescriptor container_ok;
  container_ok.data_type = PropertyDataType::kList;
  container_ok.type_ref.emplace();
  container_ok.type_ref->kind = PropertyDataType::kList;
  EXPECT_TRUE(ValidatePropertyInvariant(container_ok));

  PropertyDescriptor container_bad_no_tail;
  container_bad_no_tail.data_type = PropertyDataType::kList;
  EXPECT_FALSE(ValidatePropertyInvariant(container_bad_no_tail));

  PropertyDescriptor container_bad_mismatch;
  container_bad_mismatch.data_type = PropertyDataType::kList;
  container_bad_mismatch.type_ref.emplace();
  container_bad_mismatch.type_ref->kind = PropertyDataType::kDict;
  EXPECT_FALSE(ValidatePropertyInvariant(container_bad_mismatch));
}

}  // namespace atlas
