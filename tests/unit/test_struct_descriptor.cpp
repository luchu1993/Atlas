// Wire-format round-trip for StructDescriptor + DataTypeRef.
// Blobs are synthesised by hand (DefParser isn't wired up yet) so the
// recursive decoder and RegisterStruct's collision rules are locked in
// before any generator depends on them.

#include <cstdint>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "entitydef/entity_def_registry.h"
#include "serialization/binary_stream.h"

namespace atlas {

namespace {

// ---- Helpers to synthesise DataTypeRef bytes ----

void WriteScalar(BinaryWriter& w, PropertyDataType kind) {
  w.Write<uint8_t>(static_cast<uint8_t>(kind));
}

void WriteList(BinaryWriter& w, auto&& write_elem) {
  w.Write<uint8_t>(static_cast<uint8_t>(PropertyDataType::kList));
  write_elem(w);
}

void WriteDict(BinaryWriter& w, auto&& write_key, auto&& write_value) {
  w.Write<uint8_t>(static_cast<uint8_t>(PropertyDataType::kDict));
  write_key(w);
  write_value(w);
}

void WriteStructRef(BinaryWriter& w, uint16_t struct_id) {
  w.Write<uint8_t>(static_cast<uint8_t>(PropertyDataType::kStruct));
  w.Write<uint16_t>(struct_id);
}

// Build a StructDescriptor blob given a list of (field_name, field_type_writer).
struct FieldSpec {
  std::string_view name;
  // Writer that emits the DataTypeRef body (including leading kind byte).
  std::function<void(BinaryWriter&)> write_type;
};

auto MakeStructBlob(uint16_t struct_id, std::string_view name,
                    std::initializer_list<FieldSpec> fields) -> std::vector<std::byte> {
  BinaryWriter w;
  w.Write<uint16_t>(struct_id);
  w.WriteString(name);
  w.WritePackedInt(static_cast<uint32_t>(fields.size()));
  for (const auto& f : fields) {
    w.WriteString(f.name);
    f.write_type(w);
  }
  return w.Detach();
}

class RegistryStructTest : public ::testing::Test {
 protected:
  void SetUp() override { EntityDefRegistry::Instance().clear(); }
  void TearDown() override { EntityDefRegistry::Instance().clear(); }
};

}  // namespace

TEST_F(RegistryStructTest, ScalarOnlyStructRoundTrip) {
  auto blob = MakeStructBlob(
      /*struct_id=*/1, "ItemStack",
      {
          {"id", [](BinaryWriter& w) { WriteScalar(w, PropertyDataType::kInt32); }},
          {"count", [](BinaryWriter& w) { WriteScalar(w, PropertyDataType::kUInt16); }},
          {"bound", [](BinaryWriter& w) { WriteScalar(w, PropertyDataType::kBool); }},
      });

  ASSERT_TRUE(
      EntityDefRegistry::Instance().RegisterStruct(blob.data(), static_cast<int32_t>(blob.size())));

  const auto* s = EntityDefRegistry::Instance().FindStructById(1);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->id, 1);
  EXPECT_EQ(s->name, "ItemStack");
  ASSERT_EQ(s->fields.size(), 3u);
  EXPECT_EQ(s->fields[0].name, "id");
  EXPECT_EQ(s->fields[0].type.kind, PropertyDataType::kInt32);
  EXPECT_EQ(s->fields[1].type.kind, PropertyDataType::kUInt16);
  EXPECT_EQ(s->fields[2].type.kind, PropertyDataType::kBool);

  // Scalar fields must not carry type-ref children.
  EXPECT_EQ(s->fields[0].type.elem, nullptr);
  EXPECT_EQ(s->fields[0].type.key, nullptr);
  EXPECT_EQ(s->fields[0].type.struct_id, 0);
}

TEST_F(RegistryStructTest, ListFieldDecoded) {
  auto blob = MakeStructBlob(
      /*struct_id=*/10, "Inventory",
      {
          {"slots",
           [](BinaryWriter& w) {
             WriteList(w, [](BinaryWriter& ww) { WriteScalar(ww, PropertyDataType::kInt32); });
           }},
      });

  ASSERT_TRUE(
      EntityDefRegistry::Instance().RegisterStruct(blob.data(), static_cast<int32_t>(blob.size())));
  const auto* s = EntityDefRegistry::Instance().FindStructById(10);
  ASSERT_NE(s, nullptr);
  ASSERT_EQ(s->fields.size(), 1u);
  EXPECT_EQ(s->fields[0].type.kind, PropertyDataType::kList);
  ASSERT_NE(s->fields[0].type.elem, nullptr);
  EXPECT_EQ(s->fields[0].type.elem->kind, PropertyDataType::kInt32);
  EXPECT_EQ(s->fields[0].type.key, nullptr);
}

TEST_F(RegistryStructTest, DictFieldDecodesKeyAndValue) {
  auto blob = MakeStructBlob(
      /*struct_id=*/20, "Scoreboard",
      {
          {"scores",
           [](BinaryWriter& w) {
             WriteDict(
                 w, [](BinaryWriter& ww) { WriteScalar(ww, PropertyDataType::kString); },
                 [](BinaryWriter& ww) { WriteScalar(ww, PropertyDataType::kInt32); });
           }},
      });

  ASSERT_TRUE(
      EntityDefRegistry::Instance().RegisterStruct(blob.data(), static_cast<int32_t>(blob.size())));
  const auto* s = EntityDefRegistry::Instance().FindStructById(20);
  ASSERT_NE(s, nullptr);
  ASSERT_EQ(s->fields.size(), 1u);
  EXPECT_EQ(s->fields[0].type.kind, PropertyDataType::kDict);
  ASSERT_NE(s->fields[0].type.key, nullptr);
  ASSERT_NE(s->fields[0].type.elem, nullptr);
  EXPECT_EQ(s->fields[0].type.key->kind, PropertyDataType::kString);
  EXPECT_EQ(s->fields[0].type.elem->kind, PropertyDataType::kInt32);
}

TEST_F(RegistryStructTest, NestedListOfList) {
  auto blob = MakeStructBlob(
      /*struct_id=*/30, "Grid",
      {
          {"rows",
           [](BinaryWriter& w) {
             WriteList(w, [](BinaryWriter& ww) {
               WriteList(ww, [](BinaryWriter& www) { WriteScalar(www, PropertyDataType::kInt32); });
             });
           }},
      });

  ASSERT_TRUE(
      EntityDefRegistry::Instance().RegisterStruct(blob.data(), static_cast<int32_t>(blob.size())));
  const auto* s = EntityDefRegistry::Instance().FindStructById(30);
  ASSERT_NE(s, nullptr);
  const auto& outer = s->fields[0].type;
  EXPECT_EQ(outer.kind, PropertyDataType::kList);
  ASSERT_NE(outer.elem, nullptr);
  EXPECT_EQ(outer.elem->kind, PropertyDataType::kList);
  ASSERT_NE(outer.elem->elem, nullptr);
  EXPECT_EQ(outer.elem->elem->kind, PropertyDataType::kInt32);
}

TEST_F(RegistryStructTest, StructFieldReferencesAnotherStruct) {
  // Register ItemStack first.
  auto inner = MakeStructBlob(
      /*struct_id=*/1, "ItemStack",
      {
          {"id", [](BinaryWriter& w) { WriteScalar(w, PropertyDataType::kInt32); }},
      });
  ASSERT_TRUE(EntityDefRegistry::Instance().RegisterStruct(inner.data(),
                                                           static_cast<int32_t>(inner.size())));

  // Outer struct has field of type ItemStack (struct_id=1).
  auto outer = MakeStructBlob(
      /*struct_id=*/2, "Equipment",
      {
          {"weapon", [](BinaryWriter& w) { WriteStructRef(w, 1); }},
      });
  ASSERT_TRUE(EntityDefRegistry::Instance().RegisterStruct(outer.data(),
                                                           static_cast<int32_t>(outer.size())));

  const auto* s = EntityDefRegistry::Instance().FindStructById(2);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->fields[0].type.kind, PropertyDataType::kStruct);
  EXPECT_EQ(s->fields[0].type.struct_id, 1);
  // Follow the reference:
  const auto* ref = EntityDefRegistry::Instance().FindStructById(s->fields[0].type.struct_id);
  ASSERT_NE(ref, nullptr);
  EXPECT_EQ(ref->name, "ItemStack");
}

TEST_F(RegistryStructTest, ResolveByName) {
  auto blob = MakeStructBlob(
      /*struct_id=*/42, "SkillEntry",
      {
          {"id", [](BinaryWriter& w) { WriteScalar(w, PropertyDataType::kUInt16); }},
      });
  ASSERT_TRUE(
      EntityDefRegistry::Instance().RegisterStruct(blob.data(), static_cast<int32_t>(blob.size())));
  const auto* s = EntityDefRegistry::Instance().FindStructByName("SkillEntry");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->id, 42);
  EXPECT_EQ(EntityDefRegistry::Instance().FindStructByName("Nonexistent"), nullptr);
}

TEST_F(RegistryStructTest, MaxDepthEnforced) {
  // Build a list<list<list<...>>> with kMaxDataTypeDepth+1 nesting levels.
  // The RegisterStruct decoder must reject the blob.
  BinaryWriter w;
  w.Write<uint16_t>(99);
  w.WriteString("TooDeep");
  w.WritePackedInt(1);
  w.WriteString("deep");
  // Write nested list kinds; body emits one more elem below the depth cap,
  // then a terminal scalar. kMaxDataTypeDepth = 8, so kDepth = 9 must fail.
  constexpr std::size_t kDepth = kMaxDataTypeDepth + 1;
  for (std::size_t i = 0; i < kDepth; ++i) {
    w.Write<uint8_t>(static_cast<uint8_t>(PropertyDataType::kList));
  }
  w.Write<uint8_t>(static_cast<uint8_t>(PropertyDataType::kInt32));
  auto blob = w.Detach();

  EXPECT_FALSE(
      EntityDefRegistry::Instance().RegisterStruct(blob.data(), static_cast<int32_t>(blob.size())))
      << "Decoder must reject nesting deeper than kMaxDataTypeDepth.";
  EXPECT_EQ(EntityDefRegistry::Instance().FindStructById(99), nullptr);
}

TEST_F(RegistryStructTest, DuplicateIdSameNameReplaces) {
  auto v1 = MakeStructBlob(
      /*struct_id=*/7, "Foo",
      {
          {"a", [](BinaryWriter& w) { WriteScalar(w, PropertyDataType::kInt32); }},
      });
  ASSERT_TRUE(
      EntityDefRegistry::Instance().RegisterStruct(v1.data(), static_cast<int32_t>(v1.size())));

  auto v2 = MakeStructBlob(
      /*struct_id=*/7, "Foo",
      {
          {"a", [](BinaryWriter& w) { WriteScalar(w, PropertyDataType::kInt32); }},
          {"b", [](BinaryWriter& w) { WriteScalar(w, PropertyDataType::kFloat); }},
      });
  // Same (id, name), different field list — allowed as hot-reload replace.
  ASSERT_TRUE(
      EntityDefRegistry::Instance().RegisterStruct(v2.data(), static_cast<int32_t>(v2.size())));
  const auto* s = EntityDefRegistry::Instance().FindStructById(7);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->fields.size(), 2u) << "Re-register of (id, name) must replace, not append.";
}

TEST_F(RegistryStructTest, DuplicateIdDifferentNameRejected) {
  auto v1 = MakeStructBlob(
      /*struct_id=*/8, "Foo",
      {
          {"a", [](BinaryWriter& w) { WriteScalar(w, PropertyDataType::kInt32); }},
      });
  ASSERT_TRUE(
      EntityDefRegistry::Instance().RegisterStruct(v1.data(), static_cast<int32_t>(v1.size())));

  auto v2 = MakeStructBlob(
      /*struct_id=*/8, "Bar",  // same id, different name
      {
          {"x", [](BinaryWriter& w) { WriteScalar(w, PropertyDataType::kFloat); }},
      });
  EXPECT_FALSE(
      EntityDefRegistry::Instance().RegisterStruct(v2.data(), static_cast<int32_t>(v2.size())))
      << "id collision with different name almost certainly indicates a generator bug; reject.";

  const auto* s = EntityDefRegistry::Instance().FindStructById(8);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->name, "Foo") << "rejected registration must not mutate the existing entry.";
}

TEST_F(RegistryStructTest, ClearWipesStructs) {
  auto blob = MakeStructBlob(
      /*struct_id=*/5, "Temp",
      {
          {"v", [](BinaryWriter& w) { WriteScalar(w, PropertyDataType::kInt32); }},
      });
  ASSERT_TRUE(
      EntityDefRegistry::Instance().RegisterStruct(blob.data(), static_cast<int32_t>(blob.size())));
  EXPECT_EQ(EntityDefRegistry::Instance().StructCount(), 1u);

  EntityDefRegistry::Instance().clear();
  EXPECT_EQ(EntityDefRegistry::Instance().StructCount(), 0u);
  EXPECT_EQ(EntityDefRegistry::Instance().FindStructById(5), nullptr);
}

// ---- P1-readiness coverage: struct field types that the current codec
// must accept today so downstream passes (list / dict emitters in P1)
// aren't blocked by decoder gaps. ----------------------------------------

TEST_F(RegistryStructTest, EmptyStructIsAccepted) {
  // A zero-field struct is legal — callers might use it as a tag / marker
  // in a dict<string, TagStruct> "membership" construct. The decoder must
  // round-trip it cleanly; the original coverage skipped this case.
  auto blob = MakeStructBlob(/*struct_id=*/100, "Empty", {});
  ASSERT_TRUE(
      EntityDefRegistry::Instance().RegisterStruct(blob.data(), static_cast<int32_t>(blob.size())));

  const auto* s = EntityDefRegistry::Instance().FindStructById(100);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->name, "Empty");
  EXPECT_TRUE(s->fields.empty());
}

TEST_F(RegistryStructTest, FieldOfTypeListOfStructRef) {
  // `list<ItemStack>` as a struct field — lands as kList(elem=kStruct).
  // This is the shape that the Inventory property in the .def docs uses;
  // the P1 ObservableList<ItemStack> emitter reads the elem kind off of
  // this shape and must find the resolved struct_id.
  auto inner = MakeStructBlob(
      /*struct_id=*/1, "ItemStack",
      {
          {"id", [](BinaryWriter& w) { WriteScalar(w, PropertyDataType::kInt32); }},
      });
  ASSERT_TRUE(EntityDefRegistry::Instance().RegisterStruct(inner.data(),
                                                           static_cast<int32_t>(inner.size())));

  auto outer = MakeStructBlob(
      /*struct_id=*/2, "Inventory",
      {
          {"slots",
           [](BinaryWriter& w) { WriteList(w, [](BinaryWriter& ww) { WriteStructRef(ww, 1); }); }},
      });
  ASSERT_TRUE(EntityDefRegistry::Instance().RegisterStruct(outer.data(),
                                                           static_cast<int32_t>(outer.size())));

  const auto* s = EntityDefRegistry::Instance().FindStructById(2);
  ASSERT_NE(s, nullptr);
  const auto& slots = s->fields[0].type;
  EXPECT_EQ(slots.kind, PropertyDataType::kList);
  ASSERT_NE(slots.elem, nullptr);
  EXPECT_EQ(slots.elem->kind, PropertyDataType::kStruct);
  EXPECT_EQ(slots.elem->struct_id, 1);
}

TEST_F(RegistryStructTest, FieldOfTypeDictStringToStructRef) {
  // `dict<string, ItemStack>` — the `equipped` shape from the docs.
  // Verifies kDict(key=scalar, value=struct) decodes both sides and
  // surfaces a resolvable struct_id on the value side.
  auto inner = MakeStructBlob(
      /*struct_id=*/1, "ItemStack",
      {
          {"id", [](BinaryWriter& w) { WriteScalar(w, PropertyDataType::kInt32); }},
      });
  ASSERT_TRUE(EntityDefRegistry::Instance().RegisterStruct(inner.data(),
                                                           static_cast<int32_t>(inner.size())));

  auto outer = MakeStructBlob(
      /*struct_id=*/2, "EquippedSet",
      {
          {"items",
           [](BinaryWriter& w) {
             WriteDict(
                 w, [](BinaryWriter& ww) { WriteScalar(ww, PropertyDataType::kString); },
                 [](BinaryWriter& ww) { WriteStructRef(ww, 1); });
           }},
      });
  ASSERT_TRUE(EntityDefRegistry::Instance().RegisterStruct(outer.data(),
                                                           static_cast<int32_t>(outer.size())));

  const auto* s = EntityDefRegistry::Instance().FindStructById(2);
  ASSERT_NE(s, nullptr);
  const auto& items = s->fields[0].type;
  EXPECT_EQ(items.kind, PropertyDataType::kDict);
  ASSERT_NE(items.key, nullptr);
  ASSERT_NE(items.elem, nullptr);
  EXPECT_EQ(items.key->kind, PropertyDataType::kString);
  EXPECT_EQ(items.elem->kind, PropertyDataType::kStruct);
  EXPECT_EQ(items.elem->struct_id, 1);
}

TEST_F(RegistryStructTest, DefaultConstructedDataTypeRefIsInvalid) {
  // `PropertyDataType::kInvalid` is the explicit sentinel a fresh
  // DataTypeRef carries. Anyone who sees this at runtime is holding a
  // bug (forgot to populate type_ref / struct_id on a container prop);
  // the decoder likewise rejects it so wire-side kInvalid can't slip
  // through.
  DataTypeRef def{};
  EXPECT_EQ(def.kind, PropertyDataType::kInvalid);
}

}  // namespace atlas
