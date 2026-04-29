#ifndef ATLAS_LIB_NETWORK_BUNDLE_H_
#define ATLAS_LIB_NETWORK_BUNDLE_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "network/message.h"
#include "serialization/binary_stream.h"

namespace atlas {

inline constexpr std::size_t kMaxBundleSize = 64 * 1024;

// Writes serialise directly into the wire buffer; variable-length
// prefixes use reserve-and-backpatch to avoid an intermediate copy.
class Bundle {
 public:
  Bundle() = default;

  void StartMessage(const MessageDesc& desc);
  [[nodiscard]] auto Writer() -> BinaryWriter&;
  void EndMessage();

  template <NetworkMessage Msg>
  void AddMessage(const Msg& msg) {
    StartMessage(Msg::Descriptor());
    msg.Serialize(writer_);
    EndMessage();
  }

  [[nodiscard]] auto Finalize() -> std::vector<std::byte>;

  [[nodiscard]] auto MessageCount() const -> uint32_t { return message_count_; }
  [[nodiscard]] auto TotalSize() const -> std::size_t { return buffer_.size(); }
  [[nodiscard]] auto empty() const -> bool { return message_count_ == 0; }
  [[nodiscard]] auto HasSpace(std::size_t needed) const -> bool {
    return buffer_.size() + needed <= kMaxBundleSize;
  }

  void Clear();

 private:
  std::vector<std::byte> buffer_;
  BinaryWriter writer_;  // attached to buffer_
  uint32_t message_count_{0};
  bool writing_message_{false};
  MessageLengthStyle current_style_{};
  std::size_t length_prefix_pos_{0};
  std::size_t payload_start_{0};
};

}  // namespace atlas

#endif  // ATLAS_LIB_NETWORK_BUNDLE_H_
