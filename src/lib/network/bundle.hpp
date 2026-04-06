#pragma once

#include "network/message.hpp"
#include "serialization/binary_stream.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace atlas
{

inline constexpr std::size_t kMaxBundleSize = 64 * 1024;  // 64 KB

class Bundle
{
public:
    Bundle() = default;

    // Start writing a new message. Must call end_message() before start_message() again.
    void start_message(const MessageDesc& desc);

    // Get writer for the current message payload.
    // The returned reference is valid only between start_message() and end_message().
    [[nodiscard]] auto writer() -> BinaryWriter&;

    // Finish the current message (patches length prefix for variable-length messages)
    void end_message();

    // Convenience: write a complete typed message
    template <NetworkMessage Msg>
    void add_message(const Msg& msg)
    {
        start_message(Msg::descriptor());
        msg.serialize(payload_writer_);
        end_message();
    }

    // Finalize and return the accumulated wire data. Clears the bundle.
    [[nodiscard]] auto finalize() -> std::vector<std::byte>;

    // Query
    [[nodiscard]] auto message_count() const -> uint32_t { return message_count_; }
    [[nodiscard]] auto total_size() const -> std::size_t { return buffer_.size(); }
    [[nodiscard]] auto empty() const -> bool { return message_count_ == 0; }
    [[nodiscard]] auto has_space(std::size_t needed) const -> bool
    {
        return buffer_.size() + needed <= kMaxBundleSize;
    }

    void clear();

private:
    std::vector<std::byte> buffer_;        // accumulated wire data
    BinaryWriter payload_writer_;          // for current message payload
    uint32_t message_count_{0};
    bool writing_message_{false};          // between start/end
    MessageLengthStyle current_style_{};
    std::size_t payload_start_{0};         // offset where payload begins
};

} // namespace atlas
