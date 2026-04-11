#pragma once

#include "network/message.hpp"
#include "serialization/binary_stream.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace atlas
{

inline constexpr std::size_t kMaxBundleSize = 64 * 1024;  // 64 KB

// Messages are written directly into the bundle's wire buffer using a
// reserve-and-backpatch strategy for variable-length prefixes.  This
// eliminates the intermediate payload_writer_ copy that was here before.
class Bundle
{
public:
    Bundle() = default;

    void start_message(const MessageDesc& desc);

    // Returns a writer that appends directly into the bundle buffer.
    [[nodiscard]] auto writer() -> BinaryWriter&;

    // Patches the length prefix for variable-length messages.
    void end_message();

    template <NetworkMessage Msg>
    void add_message(const Msg& msg)
    {
        start_message(Msg::descriptor());
        msg.serialize(writer_);
        end_message();
    }

    [[nodiscard]] auto finalize() -> std::vector<std::byte>;

    [[nodiscard]] auto message_count() const -> uint32_t { return message_count_; }
    [[nodiscard]] auto total_size() const -> std::size_t { return buffer_.size(); }
    [[nodiscard]] auto empty() const -> bool { return message_count_ == 0; }
    [[nodiscard]] auto has_space(std::size_t needed) const -> bool
    {
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
