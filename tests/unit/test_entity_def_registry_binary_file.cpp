// Container-file round trip for EntityDefRegistry::RegisterFromBinaryFile.
//
// The container is pure framing around the per-record blobs that
// RegisterStruct / RegisterComponent / RegisterType already accept, so
// the test crafts a buffer in memory, optionally writes it to a temp
// file, and verifies the registry has the entries afterwards. Wire
// format spec lives in entity_def_registry.h.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "entitydef/entity_def_registry.h"
#include "serialization/binary_stream.h"

namespace atlas {

namespace {

// ---- Inner-blob helpers (mirror the per-record formats consumed by
// RegisterStruct / RegisterComponent / RegisterType) -------------------

auto MakeStructBlob(uint16_t struct_id, std::string_view name) -> std::vector<std::byte> {
  BinaryWriter w;
  w.Write<uint16_t>(struct_id);
  w.WriteString(name);
  w.WritePackedInt(1);  // 1 field
  w.WriteString("hp");
  w.Write<uint8_t>(static_cast<uint8_t>(PropertyDataType::kInt32));
  return w.Detach();
}

auto MakeComponentBlob(uint16_t component_type_id, std::string_view name)
    -> std::vector<std::byte> {
  BinaryWriter w;
  w.Write<uint16_t>(component_type_id);
  w.WriteString(name);
  w.WriteString("");  // no base
  w.Write<uint8_t>(static_cast<uint8_t>(ComponentLocality::kSynced));
  w.WritePackedInt(0);  // no properties
  return w.Detach();
}

auto MakeMinimalEntityBlob(std::string_view name, uint16_t type_id) -> std::vector<std::byte> {
  BinaryWriter w;
  w.WriteString(name);
  w.Write<uint16_t>(type_id);
  w.Write<uint8_t>(1);  // has_cell
  w.Write<uint8_t>(1);  // has_client
  w.WritePackedInt(0);  // 0 properties
  w.WritePackedInt(0);  // 0 rpcs
  w.Write<uint8_t>(0);  // no internal compression
  w.Write<uint8_t>(0);  // no external compression
  // No slot section appended.
  return w.Detach();
}

// Wraps an inner blob in the per-record [PackedInt blob_len][bytes]
// framing the container reader expects.
void AppendRecord(BinaryWriter& w, std::span<const std::byte> blob) {
  w.WritePackedInt(static_cast<uint32_t>(blob.size()));
  w.WriteBytes(blob);
}

// Builds a complete container buffer from the three section blobs.
auto BuildContainer(std::span<const std::vector<std::byte>> structs,
                    std::span<const std::vector<std::byte>> components,
                    std::span<const std::vector<std::byte>> types,
                    uint16_t version = EntityDefRegistry::kBinaryFileVersion,
                    uint32_t magic = EntityDefRegistry::kBinaryFileMagic)
    -> std::vector<std::byte> {
  BinaryWriter w;
  w.Write<uint32_t>(magic);
  w.Write<uint16_t>(version);
  w.Write<uint16_t>(0);  // flags

  w.WritePackedInt(static_cast<uint32_t>(structs.size()));
  for (const auto& s : structs) AppendRecord(w, s);

  w.WritePackedInt(static_cast<uint32_t>(components.size()));
  for (const auto& c : components) AppendRecord(w, c);

  w.WritePackedInt(static_cast<uint32_t>(types.size()));
  for (const auto& t : types) AppendRecord(w, t);

  return w.Detach();
}

class RegistryBinaryFileTest : public ::testing::Test {
 protected:
  void SetUp() override { EntityDefRegistry::Instance().clear(); }
  void TearDown() override { EntityDefRegistry::Instance().clear(); }
};

}  // namespace

TEST_F(RegistryBinaryFileTest, EmptyContainerRegistersNothing) {
  // Header-only container — three sections each declaring 0 records.
  std::vector<std::vector<std::byte>> empty;
  auto buf = BuildContainer(empty, empty, empty);

  auto result = EntityDefRegistry::Instance().RegisterFromBinaryBuffer(buf);
  ASSERT_TRUE(result) << result.Error().Message();
  EXPECT_EQ(result->structs, 0u);
  EXPECT_EQ(result->components, 0u);
  EXPECT_EQ(result->types, 0u);
  EXPECT_EQ(EntityDefRegistry::Instance().StructCount(), 0u);
  EXPECT_EQ(EntityDefRegistry::Instance().ComponentCount(), 0u);
  EXPECT_EQ(EntityDefRegistry::Instance().TypeCount(), 0u);
}

TEST_F(RegistryBinaryFileTest, RoundTripAllThreeSections) {
  std::vector<std::vector<std::byte>> structs = {MakeStructBlob(7, "Weapon")};
  std::vector<std::vector<std::byte>> components = {MakeComponentBlob(11, "AbilityComponent")};
  std::vector<std::vector<std::byte>> types = {MakeMinimalEntityBlob("Avatar", 5)};
  auto buf = BuildContainer(structs, components, types);

  auto result = EntityDefRegistry::Instance().RegisterFromBinaryBuffer(buf);
  ASSERT_TRUE(result) << result.Error().Message();
  EXPECT_EQ(result->structs, 1u);
  EXPECT_EQ(result->components, 1u);
  EXPECT_EQ(result->types, 1u);

  EXPECT_NE(EntityDefRegistry::Instance().FindStructById(7), nullptr);
  EXPECT_NE(EntityDefRegistry::Instance().FindComponentById(11), nullptr);
  EXPECT_NE(EntityDefRegistry::Instance().FindByName("Avatar"), nullptr);
}

TEST_F(RegistryBinaryFileTest, MultipleRecordsPerSection) {
  std::vector<std::vector<std::byte>> structs = {
      MakeStructBlob(1, "A"),
      MakeStructBlob(2, "B"),
      MakeStructBlob(3, "C"),
  };
  std::vector<std::vector<std::byte>> components = {
      MakeComponentBlob(10, "X"),
      MakeComponentBlob(20, "Y"),
  };
  std::vector<std::vector<std::byte>> types = {
      MakeMinimalEntityBlob("E1", 1),
      MakeMinimalEntityBlob("E2", 2),
  };
  auto buf = BuildContainer(structs, components, types);

  auto result = EntityDefRegistry::Instance().RegisterFromBinaryBuffer(buf);
  ASSERT_TRUE(result) << result.Error().Message();
  EXPECT_EQ(result->structs, 3u);
  EXPECT_EQ(result->components, 2u);
  EXPECT_EQ(result->types, 2u);
  EXPECT_EQ(EntityDefRegistry::Instance().StructCount(), 3u);
  EXPECT_EQ(EntityDefRegistry::Instance().ComponentCount(), 2u);
  EXPECT_EQ(EntityDefRegistry::Instance().TypeCount(), 2u);
}

TEST_F(RegistryBinaryFileTest, BadMagicRejected) {
  std::vector<std::vector<std::byte>> empty;
  // Wrong magic ('XYZW' little-endian) should fail without touching state.
  auto buf = BuildContainer(empty, empty, empty,
                            /*version=*/EntityDefRegistry::kBinaryFileVersion,
                            /*magic=*/0x57'5A'59'58u);
  auto result = EntityDefRegistry::Instance().RegisterFromBinaryBuffer(buf);
  EXPECT_FALSE(result);
}

TEST_F(RegistryBinaryFileTest, UnsupportedVersionRejected) {
  std::vector<std::vector<std::byte>> empty;
  auto buf = BuildContainer(empty, empty, empty, /*version=*/999);
  auto result = EntityDefRegistry::Instance().RegisterFromBinaryBuffer(buf);
  EXPECT_FALSE(result);
}

TEST_F(RegistryBinaryFileTest, MalformedRecordStopsAtFirstFailure) {
  // A struct blob short by one byte — RegisterStruct returns false, the
  // container reader bubbles that up. Subsequent sections aren't reached
  // so registry sees only the records loaded before the failure.
  auto good_struct = MakeStructBlob(1, "Good");
  auto truncated = good_struct;
  truncated.pop_back();  // drop the last field-type byte

  std::vector<std::vector<std::byte>> structs = {good_struct, truncated};
  std::vector<std::vector<std::byte>> empty;
  auto buf = BuildContainer(structs, empty, empty);
  auto result = EntityDefRegistry::Instance().RegisterFromBinaryBuffer(buf);
  EXPECT_FALSE(result);
  // Good record landed before the bad one.
  EXPECT_NE(EntityDefRegistry::Instance().FindStructById(1), nullptr);
}

TEST_F(RegistryBinaryFileTest, FilePathRoundTrip) {
  // Same payload via the file-path overload — covers the I/O wrapper
  // separately from the buffer parser.
  std::vector<std::vector<std::byte>> structs = {MakeStructBlob(42, "FileWeapon")};
  std::vector<std::vector<std::byte>> empty;
  auto buf = BuildContainer(structs, empty, empty);

  auto path = std::filesystem::temp_directory_path() / "atlas_defdump_round_trip.bin";
  {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(f) << "cannot open temp file";
    f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
  }

  auto result = EntityDefRegistry::Instance().RegisterFromBinaryFile(path);
  ASSERT_TRUE(result) << result.Error().Message();
  EXPECT_EQ(result->structs, 1u);
  EXPECT_NE(EntityDefRegistry::Instance().FindStructById(42), nullptr);

  std::filesystem::remove(path);
}

TEST_F(RegistryBinaryFileTest, NonexistentFileFails) {
  auto path = std::filesystem::temp_directory_path() / "atlas_defdump_does_not_exist.bin";
  std::filesystem::remove(path);  // ensure absent
  auto result = EntityDefRegistry::Instance().RegisterFromBinaryFile(path);
  EXPECT_FALSE(result);
}

}  // namespace atlas
