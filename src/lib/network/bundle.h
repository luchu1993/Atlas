#ifndef ATLAS_LIB_NETWORK_BUNDLE_H_
#define ATLAS_LIB_NETWORK_BUNDLE_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "network/message.h"
#include "serialization/binary_stream.h"

namespace atlas {

inline constexpr std::size_t kMaxBundleSize = 64 * 1024;  // 64 KB

// Messages are written directly into the bundle's wire buffer using a
// reserve-and-backpatch strategy for variable-length prefixes.  This
// eliminates the intermediate payload_writer_ copy that was here before.
class Bundle {
 public:
  Bundle() = default;

  void StartMessage(const MessageDesc& desc);

  // Returns a writer that appends directly into the bundle buffer.
  [[nodiscard]] auto Writer() -> BinaryWriter&;

  // Patches the length prefix for variable-length messages.
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

  void clear();

 private:
  std::vector<std::byte> buffer_;
  BinaryWriter writer_;  // writes directly into buffer_ (set via attach/detach)
  uint32_t message_count_{0};
  bool writing_message_{false};
  MessageLengthStyle current_style_{};
  std::size_t length_prefix_pos_{0};  // offset reserved for backpatching length
  std::size_t payload_start_{0};
};

}  // namespace atlas

#endif  // ATLAS_LIB_NETWORK_BUNDLE_H_
