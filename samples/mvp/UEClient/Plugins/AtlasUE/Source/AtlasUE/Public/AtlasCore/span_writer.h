#ifndef ATLAS_UE_CLIENT_CORE_SPAN_WRITER_H_
#define ATLAS_UE_CLIENT_CORE_SPAN_WRITER_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "AtlasCore/entity_view.h"

namespace atlas {

// Little-endian append cursor. Mirrors Atlas.Shared SpanWriter — primitives
// raw-LE, strings / bytes prefixed with PackedUInt32.
class SpanWriter {
 public:
  SpanWriter() = default;
  explicit SpanWriter(std::size_t reserve) { buf_.reserve(reserve); }

  template <typename T>
  void Write(const T& v) {
    const auto offset = buf_.size();
    buf_.resize(offset + sizeof(T));
    std::memcpy(buf_.data() + offset, &v, sizeof(T));
  }

  void WriteVec3(const Vec3& v) {
    Write(v.x);
    Write(v.y);
    Write(v.z);
  }

  void WriteQuat(const Quat& q) {
    Write(q.x);
    Write(q.y);
    Write(q.z);
    Write(q.w);
  }

  void WritePackedUInt32(uint32_t v) {
    if (v < 0xFE) {
      Write(static_cast<uint8_t>(v));
    } else if (v <= 0xFFFF) {
      Write(static_cast<uint8_t>(0xFE));
      Write(static_cast<uint16_t>(v));
    } else {
      Write(static_cast<uint8_t>(0xFF));
      Write(v);
    }
  }

  void WriteString(const std::string& s) {
    WritePackedUInt32(static_cast<uint32_t>(s.size()));
    const auto offset = buf_.size();
    buf_.resize(offset + s.size());
    std::memcpy(buf_.data() + offset, s.data(), s.size());
  }

  void WriteBytes(const std::vector<uint8_t>& b) {
    WritePackedUInt32(static_cast<uint32_t>(b.size()));
    const auto offset = buf_.size();
    buf_.resize(offset + b.size());
    if (!b.empty()) std::memcpy(buf_.data() + offset, b.data(), b.size());
  }

  [[nodiscard]] const std::vector<uint8_t>& Bytes() const { return buf_; }
  [[nodiscard]] std::size_t Size() const { return buf_.size(); }

 private:
  std::vector<uint8_t> buf_;
};

}  // namespace atlas

#endif  // ATLAS_UE_CLIENT_CORE_SPAN_WRITER_H_
