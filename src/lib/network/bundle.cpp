#include "network/bundle.hpp"

#include <cassert>
#include <cstring>

namespace atlas
{

void Bundle::start_message(const MessageDesc& desc)
{
    assert(!writing_message_);
    writing_message_ = true;
    current_style_ = desc.length_style;

    // Move buffer into writer so payload writes go directly into it
    writer_.attach(std::move(buffer_));

    // Write MessageID using packed encoding (1 byte if < 0xFE, else 3 bytes)
    writer_.write_packed_int(desc.id);

    if (current_style_ == MessageLengthStyle::Variable)
    {
        length_prefix_pos_ = writer_.size();
        static_cast<void>(writer_.reserve(1));  // 1-byte slot; expanded in end_message if needed
    }

    payload_start_ = writer_.size();
}

auto Bundle::writer() -> BinaryWriter&
{
    return writer_;
}

void Bundle::end_message()
{
    assert(writing_message_);
    writing_message_ = false;
    ++message_count_;

    if (current_style_ == MessageLengthStyle::Variable)
    {
        auto payload_len = static_cast<uint32_t>(writer_.size() - payload_start_);

        if (payload_len < 0xFE)
        {
            writer_.mutable_data()[length_prefix_pos_] = static_cast<std::byte>(payload_len);
        }
        else if (payload_len <= 0xFFFF)
        {
            // Tag byte becomes 0xFE; need 2 more bytes for uint16 LE
            constexpr std::size_t extra = 2;
            static_cast<void>(writer_.reserve(extra));
            auto* base = writer_.mutable_data();
            base[length_prefix_pos_] = static_cast<std::byte>(0xFE);
            auto* payload_ptr = base + payload_start_;
            std::memmove(payload_ptr + extra, payload_ptr, payload_len);
            auto le16 = endian::to_little(static_cast<uint16_t>(payload_len));
            std::memcpy(payload_ptr, &le16, sizeof(uint16_t));
        }
        else
        {
            // Tag byte becomes 0xFF; need 4 more bytes for uint32 LE
            constexpr std::size_t extra = 4;
            static_cast<void>(writer_.reserve(extra));
            auto* base = writer_.mutable_data();
            base[length_prefix_pos_] = static_cast<std::byte>(0xFF);
            auto* payload_ptr = base + payload_start_;
            std::memmove(payload_ptr + extra, payload_ptr, payload_len);
            auto le32 = endian::to_little(payload_len);
            std::memcpy(payload_ptr, &le32, sizeof(uint32_t));
        }
    }

    buffer_ = writer_.detach();
}

auto Bundle::finalize() -> std::vector<std::byte>
{
    assert(!writing_message_);
    auto result = std::move(buffer_);
    clear();
    return result;
}

void Bundle::clear()
{
    buffer_.clear();
    writer_.clear();
    message_count_ = 0;
    writing_message_ = false;
}

}  // namespace atlas
