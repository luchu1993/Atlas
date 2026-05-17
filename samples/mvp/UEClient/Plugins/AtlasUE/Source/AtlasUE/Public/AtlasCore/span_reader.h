#ifndef ATLAS_UE_CLIENT_CORE_SPAN_READER_H_
#define ATLAS_UE_CLIENT_CORE_SPAN_READER_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "AtlasCore/entity_view.h"

namespace atlas {

// Little-endian cursor over a borrowed byte span; Atlas wire is fixed LE
// and all current targets are LE.
class SpanReader {
 public:
  SpanReader(const uint8_t* data, std::size_t len) : data_(data), len_(len) {}

  [[nodiscard]] bool Has(std::size_t n) const { return pos_ + n <= len_; }
  [[nodiscard]] std::size_t Pos() const { return pos_; }
  [[nodiscard]] std::size_t Remaining() const { return len_ - pos_; }
  [[nodiscard]] const uint8_t* Data() const { return data_; }

  template <typename T>
  bool Read(T& out) {
    if (!Has(sizeof(T))) return false;
    std::memcpy(&out, data_ + pos_, sizeof(T));
    pos_ += sizeof(T);
    return true;
  }

  bool ReadVec3(Vec3& v) { return Read(v.x) && Read(v.y) && Read(v.z); }
  bool ReadQuat(Quat& q) { return Read(q.x) && Read(q.y) && Read(q.z) && Read(q.w); }

  // Mirror of Atlas.Shared SpanReader.ReadPackedUInt32: 3-tier varint
  // (0x00..0xFD inline, 0xFE → u16 tail, 0xFF → u32 tail).
  bool ReadPackedUInt32(uint32_t& out) {
    uint8_t tag;
    if (!Read(tag)) return false;
    if (tag < 0xFE) { out = tag; return true; }
    if (tag == 0xFE) {
      uint16_t v;
      if (!Read(v)) return false;
      out = v;
      return true;
    }
    return Read(out);
  }

  bool ReadString(std::string& out) {
    uint32_t len;
    if (!ReadPackedUInt32(len)) return false;
    if (!Has(len)) return false;
    out.assign(reinterpret_cast<const char*>(data_ + pos_), len);
    return Skip(len);
  }

  bool ReadBytes(std::vector<uint8_t>& out) {
    uint32_t len;
    if (!ReadPackedUInt32(len)) return false;
    if (!Has(len)) return false;
    out.assign(data_ + pos_, data_ + pos_ + len);
    return Skip(len);
  }

  bool Skip(std::size_t n) {
    if (!Has(n)) return false;
    pos_ += n;
    return true;
  }

 private:
  const uint8_t* data_;
  std::size_t len_;
  std::size_t pos_{0};
};

}  // namespace atlas

#endif  // ATLAS_UE_CLIENT_CORE_SPAN_READER_H_
