#include "entitydef/entitydef_api.h"

#include <cstddef>
#include <filesystem>
#include <new>
#include <span>
#include <string>

#include "entitydef/entity_def_registry.h"

namespace {

thread_local std::string g_global_last_error;

auto AbiCompatible(uint32_t expected) -> bool {
  constexpr uint32_t kOur = ATLAS_EDR_ABI_VERSION;
  const uint32_t our_major = (kOur >> 24) & 0xFFu;
  const uint32_t our_minor = (kOur >> 16) & 0xFFu;
  const uint32_t exp_major = (expected >> 24) & 0xFFu;
  const uint32_t exp_minor = (expected >> 16) & 0xFFu;
  return exp_major == our_major && exp_minor <= our_minor;
}

}  // namespace

struct AtlasEdrContext {
  atlas::EntityDefRegistry registry;
  std::string last_error;
  bool loaded{false};
};

extern "C" {

uint32_t AtlasEdrGetAbiVersion(void) { return ATLAS_EDR_ABI_VERSION; }

const char* AtlasEdrLastError(AtlasEdrContext* ctx) {
  if (!ctx) return AtlasEdrGlobalLastError();
  return ctx->last_error.empty() ? "" : ctx->last_error.c_str();
}

const char* AtlasEdrGlobalLastError(void) {
  return g_global_last_error.empty() ? "" : g_global_last_error.c_str();
}

AtlasEdrContext* AtlasEdrCreate(uint32_t expected_abi) {
  if (!AbiCompatible(expected_abi)) {
    g_global_last_error = "ABI mismatch";
    return nullptr;
  }
  try {
    return new AtlasEdrContext{};
  } catch (const std::bad_alloc&) {
    g_global_last_error = "out of memory";
    return nullptr;
  }
}

void AtlasEdrDestroy(AtlasEdrContext* ctx) { delete ctx; }

int32_t AtlasEdrLoadFromFile(AtlasEdrContext* ctx, const char* path) {
  if (!ctx || !path) return ATLAS_EDR_ERR_INVAL;
  auto result = ctx->registry.RegisterFromBinaryFile(std::filesystem::path(path));
  if (!result.HasValue()) {
    ctx->last_error = std::string(result.Error().Message());
    return ATLAS_EDR_ERR_PARSE;
  }
  ctx->loaded = true;
  return ATLAS_EDR_OK;
}

int32_t AtlasEdrLoadFromBuffer(AtlasEdrContext* ctx, const uint8_t* data, int32_t len) {
  if (!ctx || !data || len <= 0) return ATLAS_EDR_ERR_INVAL;
  auto buf = std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(data), static_cast<size_t>(len));
  auto result = ctx->registry.RegisterFromBinaryBuffer(buf);
  if (!result.HasValue()) {
    ctx->last_error = std::string(result.Error().Message());
    return ATLAS_EDR_ERR_PARSE;
  }
  ctx->loaded = true;
  return ATLAS_EDR_OK;
}

const uint8_t* AtlasEdrGetDigest(AtlasEdrContext* ctx) {
  if (!ctx || !ctx->loaded) return nullptr;
  return ctx->registry.Digest().data();
}

int32_t AtlasEdrGetDigestSize(AtlasEdrContext* ctx) {
  if (!ctx || !ctx->loaded) return 0;
  return static_cast<int32_t>(ctx->registry.Digest().size());
}

}  // extern "C"
