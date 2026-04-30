#include "network/bundle.h"

#include <cassert>
#include <cstring>

namespace atlas {

void Bundle::StartMessage(const MessageDesc& desc) {
  assert(!writing_message_);
  writing_message_ = true;
  current_style_ = desc.length_style;

  writer_.Attach(std::move(buffer_));

  writer_.WritePackedInt(desc.id);

  if (current_style_ == MessageLengthStyle::kVariable) {
    length_prefix_pos_ = writer_.Size();
    static_cast<void>(writer_.Reserve(1));
  }

  payload_start_ = writer_.Size();
}

auto Bundle::Writer() -> BinaryWriter& {
  return writer_;
}

void Bundle::EndMessage() {
  assert(writing_message_);
  writing_message_ = false;
  ++message_count_;

  if (current_style_ == MessageLengthStyle::kVariable) {
    auto payload_len = static_cast<uint32_t>(writer_.Size() - payload_start_);

    if (payload_len < 0xFE) {
      writer_.MutableData()[length_prefix_pos_] = static_cast<std::byte>(payload_len);
    } else if (payload_len <= 0xFFFF) {
      constexpr std::size_t kExtra = 2;
      static_cast<void>(writer_.Reserve(kExtra));
      auto* base = writer_.MutableData();
      base[length_prefix_pos_] = static_cast<std::byte>(0xFE);
      auto* payload_ptr = base + payload_start_;
      std::memmove(payload_ptr + kExtra, payload_ptr, payload_len);
      auto le16 = endian::ToLittle(static_cast<uint16_t>(payload_len));
      std::memcpy(payload_ptr, &le16, sizeof(uint16_t));
    } else {
      constexpr std::size_t kExtra = 4;
      static_cast<void>(writer_.Reserve(kExtra));
      auto* base = writer_.MutableData();
      base[length_prefix_pos_] = static_cast<std::byte>(0xFF);
      auto* payload_ptr = base + payload_start_;
      std::memmove(payload_ptr + kExtra, payload_ptr, payload_len);
      auto le32 = endian::ToLittle(payload_len);
      std::memcpy(payload_ptr, &le32, sizeof(uint32_t));
    }
  }

  buffer_ = writer_.Detach();
}

auto Bundle::Finalize() -> std::vector<std::byte> {
  assert(!writing_message_);
  auto result = std::move(buffer_);
  Clear();
  return result;
}

void Bundle::Clear() {
  buffer_.clear();
  writer_.Clear();
  message_count_ = 0;
  writing_message_ = false;
}

}  // namespace atlas
