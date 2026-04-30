#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

#include "entitydef/entity_def_registry.h"
#include "entitydef/entity_type_descriptor.h"
#include "serialization/binary_stream.h"

// Binary-descriptor fixture for the canonical "Account" entity used across
// DBApp integration tests (type_id=1, identifier="accountName" string,
// "level" int32, both persistent base-only). The blob format mirrors what
// EntityDefRegistry::RegisterType reads on the wire — keeping it in lockstep
// with the codegen-emitted Build_<Foo> output is the whole point of the
// dual-emit pipeline.
//
// Lives in a header so tests don't need a separate translation unit; helpers
// are inline. Each test should call WriteAccountAtdfFile once at startup and
// pass the returned path to DBApp via --entitydef-bin-path.

namespace atlas::test_fixtures {

inline auto MakeAccountTypeBlob() -> std::vector<std::byte> {
  BinaryWriter w;
  w.WriteString("Account");
  w.Write<uint16_t>(1);  // type_id
  w.Write<uint8_t>(0);   // has_cell = false
  w.Write<uint8_t>(1);   // has_client = true
  w.WritePackedInt(2);   // 2 properties

  // Property 0: accountName (string, persistent, identifier, base-only)
  w.WriteString("accountName");
  w.Write<uint8_t>(static_cast<uint8_t>(PropertyDataType::kString));
  w.Write<uint8_t>(static_cast<uint8_t>(ReplicationScope::kBase));
  w.Write<uint8_t>(1);   // persistent
  w.Write<uint8_t>(5);   // detail_level
  w.Write<uint16_t>(0);  // index
  w.Write<uint8_t>(1);   // identifier
  w.Write<uint8_t>(0);   // reliable

  // Property 1: level (int32, persistent, base-only)
  w.WriteString("level");
  w.Write<uint8_t>(static_cast<uint8_t>(PropertyDataType::kInt32));
  w.Write<uint8_t>(static_cast<uint8_t>(ReplicationScope::kBase));
  w.Write<uint8_t>(1);   // persistent
  w.Write<uint8_t>(5);   // detail_level
  w.Write<uint16_t>(1);  // index
  w.Write<uint8_t>(0);   // identifier
  w.Write<uint8_t>(0);   // reliable

  w.WritePackedInt(0);  // rpc_count
  return w.Detach();
}

inline auto WriteAccountAtdfFile(const std::filesystem::path& path) -> std::filesystem::path {
  auto type_blob = MakeAccountTypeBlob();

  BinaryWriter w;
  w.Write<uint32_t>(EntityDefRegistry::kBinaryFileMagic);
  w.Write<uint16_t>(EntityDefRegistry::kBinaryFileVersion);
  w.Write<uint16_t>(0);  // flags
  w.WritePackedInt(0);   // 0 structs
  w.WritePackedInt(0);   // 0 components
  w.WritePackedInt(1);   // 1 type
  w.WritePackedInt(static_cast<uint32_t>(type_blob.size()));
  w.WriteBytes(std::span<const std::byte>(type_blob.data(), type_blob.size()));

  auto bytes = w.Detach();
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return path;
}

}  // namespace atlas::test_fixtures
