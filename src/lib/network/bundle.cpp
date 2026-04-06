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

    // Write MessageID in little-endian
    auto id_le = endian::to_little(desc.id);
    const auto* id_bytes = reinterpret_cast<const std::byte*>(&id_le);
    buffer_.insert(buffer_.end(), id_bytes, id_bytes + sizeof(uint16_t));

    // Record where payload will begin (after we write the length prefix in end_message)
    payload_start_ = buffer_.size();

    // Reset payload writer for this message
    payload_writer_.clear();
}

auto Bundle::writer() -> BinaryWriter&
{
    return payload_writer_;
}

void Bundle::end_message()
{
    assert(writing_message_);
    writing_message_ = false;
    ++message_count_;

    auto payload = payload_writer_.data();

    if (current_style_ == MessageLengthStyle::Variable)
    {
        // Write packed_int length prefix, then payload
        BinaryWriter len_writer;
        len_writer.write_packed_int(static_cast<uint32_t>(payload.size()));
        auto len_data = len_writer.data();
        buffer_.insert(buffer_.end(), len_data.begin(), len_data.end());
    }

    // Append payload data
    buffer_.insert(buffer_.end(), payload.begin(), payload.end());
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
    payload_writer_.clear();
    message_count_ = 0;
    writing_message_ = false;
}

} // namespace atlas
