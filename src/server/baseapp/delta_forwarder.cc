#include "delta_forwarder.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#include "baseapp_messages.h"
#include "network/channel.h"
#include "protocol/aoi_envelope.h"

namespace atlas {

namespace {

constexpr std::size_t kEnvelopeHeaderBytes = 1 + sizeof(EntityID);
constexpr std::size_t kPositionUpdateBytes = 6 * sizeof(float) + 1 + sizeof(double);
constexpr std::size_t kPositionEnvelopeBytes = kEnvelopeHeaderBytes + kPositionUpdateBytes;
constexpr float kMaxDirectionY = 0.01f;
constexpr float kMaxDirectionLengthError = 0.01f;
constexpr float kMinDirectionLengthSq = 0.000001f;
constexpr double kTwoPi = 6.28318530717958647692;
constexpr double kTimeOffsetScale = 1000.0;

struct PositionDelta {
  EntityID entity_id{kInvalidEntityID};
  float px{0.0f};
  float py{0.0f};
  float pz{0.0f};
  float dx{0.0f};
  float dy{0.0f};
  float dz{0.0f};
  bool on_ground{false};
  double server_time{0.0};
};

struct EncodedPositionEntry {
  uint32_t entity_delta{0};
  int16_t qx{0};
  int16_t qy{0};
  int16_t qz{0};
  uint8_t yaw{0};
  uint16_t time_offset_ms{0};
  bool on_ground{false};
};

[[nodiscard]] auto CanUsePackedXZ(int16_t value) -> bool {
  return value >= kPositionBatchMinPackedXZOffset &&
         value <= kPositionBatchMaxPackedXZOffset;
}

[[nodiscard]] auto PackSigned12(int16_t value) -> uint32_t {
  return static_cast<uint32_t>(static_cast<uint16_t>(value)) & 0x0FFFu;
}

void WritePackedXZ(BinaryWriter& writer, int16_t qx, int16_t qz) {
  const uint32_t packed = PackSigned12(qx) | (PackSigned12(qz) << 12);
  writer.Write<uint8_t>(static_cast<uint8_t>(packed & 0xFFu));
  writer.Write<uint8_t>(static_cast<uint8_t>((packed >> 8) & 0xFFu));
  writer.Write<uint8_t>(static_cast<uint8_t>((packed >> 16) & 0xFFu));
}

[[nodiscard]] auto DirectionCanUseYaw(const PositionDelta& delta) -> bool {
  if (!std::isfinite(delta.dx) || !std::isfinite(delta.dy) || !std::isfinite(delta.dz)) {
    return false;
  }
  if (std::abs(delta.dy) > kMaxDirectionY) return false;
  const float xz_len_sq = delta.dx * delta.dx + delta.dz * delta.dz;
  if (xz_len_sq < kMinDirectionLengthSq) return false;
  const float xz_len = std::sqrt(xz_len_sq);
  return std::abs(xz_len - 1.0f) <= kMaxDirectionLengthError;
}

[[nodiscard]] auto DecodePositionDelta(std::span<const std::byte> bytes,
                                       EntityID expected_entity_id)
    -> std::optional<PositionDelta> {
  if (bytes.size() != kPositionEnvelopeBytes) return std::nullopt;

  BinaryReader reader(bytes);
  auto kind = reader.Read<uint8_t>();
  auto entity_id = reader.Read<EntityID>();
  auto px = reader.Read<float>();
  auto py = reader.Read<float>();
  auto pz = reader.Read<float>();
  auto dx = reader.Read<float>();
  auto dy = reader.Read<float>();
  auto dz = reader.Read<float>();
  auto on_ground = reader.Read<uint8_t>();
  auto server_time = reader.Read<double>();
  if (!kind || !entity_id || !px || !py || !pz || !dx || !dy || !dz || !on_ground || !server_time ||
      reader.Remaining() != 0) {
    return std::nullopt;
  }
  if (*kind != static_cast<uint8_t>(CellAoIEnvelopeKind::kEntityPositionUpdate)) {
    return std::nullopt;
  }
  if (*entity_id != expected_entity_id) return std::nullopt;

  PositionDelta delta{*entity_id, *px, *py, *pz, *dx, *dy, *dz, *on_ground != 0, *server_time};
  if (!std::isfinite(delta.px) || !std::isfinite(delta.py) || !std::isfinite(delta.pz) ||
      !std::isfinite(delta.server_time)) {
    return std::nullopt;
  }
  if (!DirectionCanUseYaw(delta)) return std::nullopt;
  return delta;
}

[[nodiscard]] auto QuantizeOffset(float value) -> std::optional<int16_t> {
  const double scaled = std::round(static_cast<double>(value) * kPositionBatchScale);
  if (!std::isfinite(scaled)) return std::nullopt;
  if (scaled < std::numeric_limits<int16_t>::min() ||
      scaled > std::numeric_limits<int16_t>::max()) {
    return std::nullopt;
  }
  return static_cast<int16_t>(scaled);
}

[[nodiscard]] auto QuantizeYaw(const PositionDelta& delta) -> uint8_t {
  double yaw = std::atan2(static_cast<double>(delta.dx), static_cast<double>(delta.dz));
  if (yaw < 0.0) yaw += kTwoPi;
  const auto quantized = static_cast<uint32_t>(std::llround(yaw / kTwoPi * 256.0)) & 0xFFu;
  return static_cast<uint8_t>(quantized);
}

[[nodiscard]] auto QuantizeTimeOffset(double server_time, double base_time)
    -> std::optional<uint16_t> {
  const double scaled = std::round((server_time - base_time) * kTimeOffsetScale);
  if (!std::isfinite(scaled) || scaled < 0.0 ||
      scaled > std::numeric_limits<uint16_t>::max()) {
    return std::nullopt;
  }
  return static_cast<uint16_t>(scaled);
}

[[nodiscard]] auto BuildPositionBatch(std::span<const PositionDelta> entries)
    -> std::optional<std::vector<std::byte>> {
  if (entries.size() < 2 || entries.size() > std::numeric_limits<uint16_t>::max()) {
    return std::nullopt;
  }

  std::vector<PositionDelta> sorted(entries.begin(), entries.end());
  std::sort(sorted.begin(), sorted.end(), [](const PositionDelta& a, const PositionDelta& b) {
    return a.entity_id < b.entity_id;
  });

  float min_x = sorted.front().px;
  float min_y = sorted.front().py;
  float min_z = sorted.front().pz;
  float max_x = min_x;
  float max_y = min_y;
  float max_z = min_z;
  double base_time = sorted.front().server_time;
  for (const auto& entry : sorted) {
    min_x = std::min(min_x, entry.px);
    min_y = std::min(min_y, entry.py);
    min_z = std::min(min_z, entry.pz);
    max_x = std::max(max_x, entry.px);
    max_y = std::max(max_y, entry.py);
    max_z = std::max(max_z, entry.pz);
    base_time = std::min(base_time, entry.server_time);
  }

  const float origin_x = (min_x + max_x) * 0.5f;
  const float origin_y = (min_y + max_y) * 0.5f;
  const float origin_z = (min_z + max_z) * 0.5f;
  if (!std::isfinite(origin_x) || !std::isfinite(origin_y) || !std::isfinite(origin_z)) {
    return std::nullopt;
  }

  std::vector<EncodedPositionEntry> encoded;
  encoded.reserve(sorted.size());
  bool has_time_offsets = false;
  bool has_y_offsets = false;
  bool has_wide_xz_offsets = false;
  bool all_on_ground = true;
  bool any_on_ground = false;
  bool sequential_entity_ids = true;
  EntityID previous_id = sorted.front().entity_id;
  for (std::size_t i = 0; i < sorted.size(); ++i) {
    const auto& entry = sorted[i];
    const uint32_t entity_delta = i == 0 ? 0 : entry.entity_id - previous_id;
    auto qx = QuantizeOffset(entry.px - origin_x);
    auto qy = QuantizeOffset(entry.py - origin_y);
    auto qz = QuantizeOffset(entry.pz - origin_z);
    auto time_offset_ms = QuantizeTimeOffset(entry.server_time, base_time);
    if (!qx || !qy || !qz || !time_offset_ms) return std::nullopt;

    has_time_offsets = has_time_offsets || *time_offset_ms != 0;
    has_y_offsets = has_y_offsets || *qy != 0;
    has_wide_xz_offsets =
        has_wide_xz_offsets || !CanUsePackedXZ(*qx) || !CanUsePackedXZ(*qz);
    all_on_ground = all_on_ground && entry.on_ground;
    any_on_ground = any_on_ground || entry.on_ground;
    sequential_entity_ids =
        sequential_entity_ids && entity_delta == (i == 0 ? 0u : 1u);
    encoded.push_back(EncodedPositionEntry{entity_delta, *qx, *qy, *qz, QuantizeYaw(entry),
                                           *time_offset_ms, entry.on_ground});
    previous_id = entry.entity_id;
  }

  const bool has_on_ground_bits = any_on_ground && !all_on_ground;
  uint8_t flags = 0;
  if (has_time_offsets) flags |= kPositionBatchHasTimeOffsets;
  if (has_on_ground_bits) flags |= kPositionBatchHasOnGroundBits;
  if (all_on_ground) flags |= kPositionBatchAllOnGround;
  if (has_y_offsets) flags |= kPositionBatchHasYOffsets;
  if (has_wide_xz_offsets) flags |= kPositionBatchHasWideXZOffsets;
  if (sequential_entity_ids) flags |= kPositionBatchSequentialEntityIds;

  const std::size_t on_ground_bytes = has_on_ground_bits ? (encoded.size() + 7) / 8 : 0;
  constexpr std::size_t kBatchHeaderBytes =
      kEnvelopeHeaderBytes + 3 * sizeof(float) + sizeof(double) + sizeof(uint16_t) + 2;
  constexpr std::size_t kBatchEntryFixedBytes = sizeof(uint8_t);
  const std::size_t entity_delta_bytes = sequential_entity_ids ? 0 : encoded.size();
  const std::size_t xz_bytes =
      encoded.size() * (has_wide_xz_offsets ? 2 * sizeof(int16_t)
                                             : kPositionBatchPackedXZBytes);
  const std::size_t y_bytes = has_y_offsets ? encoded.size() * sizeof(int16_t) : 0;
  const std::size_t time_bytes =
      has_time_offsets ? encoded.size() * sizeof(uint16_t) : 0;
  BinaryWriter writer(kBatchHeaderBytes + on_ground_bytes +
                      encoded.size() * kBatchEntryFixedBytes + entity_delta_bytes + xz_bytes +
                      y_bytes + time_bytes);
  writer.Write<uint8_t>(static_cast<uint8_t>(CellAoIEnvelopeKind::kEntityPositionBatch));
  writer.Write<EntityID>(0);
  writer.Write<float>(origin_x);
  writer.Write<float>(origin_y);
  writer.Write<float>(origin_z);
  writer.Write<double>(base_time);
  writer.Write<uint16_t>(static_cast<uint16_t>(sorted.size()));
  writer.Write<uint8_t>(flags);
  writer.WritePackedInt(sorted.front().entity_id);

  if (has_on_ground_bits) {
    for (std::size_t base = 0; base < encoded.size(); base += 8) {
      uint8_t bits = 0;
      for (std::size_t bit = 0; bit < 8 && base + bit < encoded.size(); ++bit) {
        if (encoded[base + bit].on_ground) bits |= static_cast<uint8_t>(1u << bit);
      }
      writer.Write<uint8_t>(bits);
    }
  }

  for (const auto& entry : encoded) {
    if (!sequential_entity_ids) writer.WritePackedInt(entry.entity_delta);
    if (has_wide_xz_offsets) {
      writer.Write<int16_t>(entry.qx);
      writer.Write<int16_t>(entry.qz);
    } else {
      WritePackedXZ(writer, entry.qx, entry.qz);
    }
    if (has_y_offsets) writer.Write<int16_t>(entry.qy);
    writer.Write<uint8_t>(entry.yaw);
    if (has_time_offsets) writer.Write<uint16_t>(entry.time_offset_ms);
  }
  return writer.Detach();
}

void SendClientDelta(Channel& client_ch, std::span<const std::byte> bytes) {
  (void)client_ch.SendMessage(baseapp::ClientDeltaEnvelope{bytes});
}

}  // namespace

void DeltaForwarder::Enqueue(EntityID entity_id, std::span<const std::byte> delta,
                             uint16_t priority) {
  // Latest-wins: replace payload, keep accumulated deferred_ticks, max-merge priority.
  for (auto& entry : queue_) {
    if (entry.entity_id == entity_id) {
      entry.delta.assign(delta.begin(), delta.end());
      entry.priority = std::max(entry.priority, priority);
      return;
    }
  }

  queue_.push_back(PendingDelta{entity_id, {delta.begin(), delta.end()}, 0, priority});
}

auto DeltaForwarder::SendDeltas(Channel& client_ch, std::span<const PendingDelta> entries)
    -> uint32_t {
  if (entries.empty()) return 0;

  std::vector<PositionDelta> position_entries;
  std::vector<std::size_t> position_indexes;
  position_entries.reserve(entries.size());
  position_indexes.reserve(entries.size());
  for (std::size_t i = 0; i < entries.size(); ++i) {
    auto decoded = DecodePositionDelta(entries[i].delta, entries[i].entity_id);
    if (!decoded) continue;
    position_indexes.push_back(i);
    position_entries.push_back(*decoded);
  }

  uint32_t bytes_sent = 0;
  std::vector<bool> sent_as_batch(entries.size(), false);
  auto batch = BuildPositionBatch(position_entries);
  std::size_t batch_insert_index = entries.size();
  if (batch) {
    batch_insert_index = position_indexes.front();
    for (auto index : position_indexes) sent_as_batch[index] = true;
  }

  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (i == batch_insert_index) {
      SendClientDelta(client_ch, std::span<const std::byte>(batch->data(), batch->size()));
      bytes_sent += static_cast<uint32_t>(batch->size());
    }
    if (sent_as_batch[i]) continue;
    const auto& delta = entries[i].delta;
    SendClientDelta(client_ch, std::span<const std::byte>(delta.data(), delta.size()));
    bytes_sent += static_cast<uint32_t>(delta.size());
  }
  return bytes_sent;
}

auto DeltaForwarder::Flush(Channel& client_ch, uint32_t budget_bytes) -> uint32_t {
  if (queue_.empty()) return 0;

  uint32_t bytes_sent = 0;

  // Pass 1: force-send starved entries regardless of budget/priority.
  auto starved_begin = std::partition(queue_.begin(), queue_.end(), [](const PendingDelta& e) {
    return e.deferred_ticks < kMaxDeferredTicks;
  });
  if (starved_begin != queue_.end()) {
    const auto starved_count = static_cast<std::size_t>(std::distance(starved_begin, queue_.end()));
    bytes_sent +=
        SendDeltas(client_ch, std::span<const PendingDelta>(&*starved_begin, starved_count));
    stats_.force_sent_count += starved_count;
  }
  queue_.erase(starved_begin, queue_.end());

  // Pass 2: sort by (priority, deferred_ticks) desc; budget is fresh - Pass 1
  // sends do not consume it because starvation already overrode it.
  std::sort(queue_.begin(), queue_.end(), [](const PendingDelta& a, const PendingDelta& b) {
    if (a.priority != b.priority) return a.priority > b.priority;
    return a.deferred_ticks > b.deferred_ticks;
  });

  uint32_t pass2_bytes = 0;
  std::size_t sent_count = 0;
  for (auto& entry : queue_) {
    auto entry_size = static_cast<uint32_t>(entry.delta.size());
    if (pass2_bytes + entry_size > budget_bytes && sent_count > 0) {
      break;
    }
    pass2_bytes += entry_size;
    ++sent_count;
  }
  if (sent_count > 0) {
    bytes_sent += SendDeltas(client_ch, std::span<const PendingDelta>(queue_.data(), sent_count));
  }
  stats_.bytes_sent += bytes_sent;

  uint64_t deferred_bytes = 0;
  for (std::size_t i = sent_count; i < queue_.size(); ++i) {
    ++queue_[i].deferred_ticks;
    deferred_bytes += queue_[i].delta.size();
  }
  stats_.bytes_deferred += deferred_bytes;

  queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(sent_count));
  return bytes_sent;
}

}  // namespace atlas
