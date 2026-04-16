#include "network/tcp_channel.h"

#include <array>
#include <cstring>

#include "foundation/log.h"
#include "network/event_dispatcher.h"

namespace atlas {

TcpChannel::TcpChannel(EventDispatcher& dispatcher, InterfaceTable& table, Socket socket,
                       const Address& remote)
    : Channel(dispatcher, table, remote), socket_(std::move(socket)) {}

TcpChannel::~TcpChannel() {
  CancelRecvBufferShrink();
  CancelWriteBufferShrink();

  if (socket_.IsValid()) {
    (void)dispatcher_.Deregister(socket_.Fd());
    socket_.Close();
  }
}

// ============================================================================
// Receive path
// ============================================================================

void TcpChannel::OnReadable() {
  std::size_t reads = 0;
  while (true) {
    if (reads >= kMaxSocketReadsPerCallback) {
      break;
    }

    auto span = recv_buffer_.WritableSpan();
    if (span.empty()) {
      if (!EnsureRecvWritable()) {
        ATLAS_LOG_ERROR("TcpChannel recv buffer full from {}", remote_.ToString());
        OnDisconnect();
        return;
      }
      span = recv_buffer_.WritableSpan();
    }

    auto result = socket_.Recv(span);
    if (!result) {
      if (result.Error().Code() == ErrorCode::kWouldBlock) {
        break;
      }
      ATLAS_LOG_WARNING("TcpChannel recv error from {}: {}", remote_.ToString(),
                        result.Error().Message());
      OnDisconnect();
      return;
    }

    if (*result == 0) {
      OnDisconnect();
      return;
    }

    CancelRecvBufferShrink();
    recv_buffer_.Commit(*result);
    OnDataReceived(std::span<const std::byte>(span.data(), *result));
    ++reads;
  }

  ProcessRecvBuffer();
}

auto TcpChannel::EnsureRecvWritable() -> bool {
  std::size_t needed = 1;

  if (recv_buffer_.ReadableSize() >= kFrameHeaderSize) {
    auto frame_length = PeekFrameLength();
    if (!frame_length) {
      return false;
    }
    if (*frame_length > kMaxBundleSize) {
      ATLAS_LOG_ERROR("TcpChannel oversized frame {} from {}", *frame_length, remote_.ToString());
      return false;
    }

    auto total = kFrameHeaderSize + static_cast<std::size_t>(*frame_length);
    if (total > recv_buffer_.ReadableSize()) {
      needed = total - recv_buffer_.ReadableSize();
    }
  }

  return recv_buffer_.EnsureWritable(needed);
}

auto TcpChannel::PeekFrameLength() const -> std::optional<uint32_t> {
  if (recv_buffer_.ReadableSize() < kFrameHeaderSize) {
    return std::nullopt;
  }

  std::array<std::byte, kFrameHeaderSize> header{};
  if (!recv_buffer_.PeekFront(header)) {
    return std::nullopt;
  }

  uint32_t frame_length = 0;
  std::memcpy(&frame_length, header.data(), sizeof(frame_length));
  return endian::FromLittle(frame_length);
}

void TcpChannel::ProcessRecvBuffer() {
  while (true) {
    auto available = recv_buffer_.ReadableSize();

    if (available < kFrameHeaderSize) {
      break;
    }

    auto frame_length = PeekFrameLength();
    if (!frame_length) {
      break;
    }

    if (*frame_length > kMaxBundleSize) {
      ATLAS_LOG_ERROR("TcpChannel oversized frame {} from {}", *frame_length, remote_.ToString());
      OnDisconnect();
      return;
    }

    std::size_t total = kFrameHeaderSize + static_cast<std::size_t>(*frame_length);
    if (available < total) {
      auto remaining = total - available;
      if (!recv_buffer_.EnsureWritable(remaining)) {
        ATLAS_LOG_ERROR("TcpChannel cannot grow recv buffer to {} bytes for {}", total,
                        remote_.ToString());
        OnDisconnect();
        return;
      }
      break;
    }

    auto rspan = recv_buffer_.ReadableSpan();
    // Ensure contiguous data for dispatch
    if (rspan.size() < total) {
      recv_buffer_.Linearize();
      rspan = recv_buffer_.ReadableSpan();
    }

    const std::byte* frame_start = rspan.data() + kFrameHeaderSize;

    if (packet_filter_) {
      auto filtered = packet_filter_->RecvFilter(
          std::span<const std::byte>(frame_start, static_cast<std::size_t>(*frame_length)));
      if (filtered) {
        DispatchMessages(std::span<const std::byte>(filtered->data(), filtered->size()));
      } else {
        ATLAS_LOG_WARNING("TcpChannel recv_filter failed from {}: {}", remote_.ToString(),
                          filtered.Error().Message());
      }
    } else {
      DispatchMessages(
          std::span<const std::byte>(frame_start, static_cast<std::size_t>(*frame_length)));
    }

    recv_buffer_.Consume(total);
  }

  if (recv_buffer_.ReadableSize() == 0) {
    ScheduleRecvBufferShrink();
  } else {
    CancelRecvBufferShrink();
  }
}

// ============================================================================
// Write path
// ============================================================================

void TcpChannel::OnWritable() {
  TryFlushWriteBuffer();
}

auto TcpChannel::DoSend(std::span<const std::byte> data) -> Result<size_t> {
  uint32_t frame_length = endian::ToLittle(static_cast<uint32_t>(data.size()));
  auto header = std::span<const std::byte>(reinterpret_cast<const std::byte*>(&frame_length),
                                           sizeof(uint32_t));

  if (!write_buffer_.EnsureWritable(header.size() + data.size())) {
    return Error(ErrorCode::kWouldBlock, "Write buffer full");
  }

  CancelWriteBufferShrink();
  if (!write_buffer_.Append(header) || !write_buffer_.Append(data)) {
    return Error(ErrorCode::kIoError, "Write buffer append failed after reserve");
  }

  TryFlushWriteBuffer();
  return static_cast<size_t>(data.size());
}

void TcpChannel::OnCondemned() {
  CancelRecvBufferShrink();
  CancelWriteBufferShrink();

  // Deregister from IOPoller to prevent stale events on condemned channels.
  // The destructor also deregisters, but doing it here avoids the window
  // between condemn and destruction where the poller could still fire.
  if (socket_.IsValid()) {
    (void)dispatcher_.Deregister(socket_.Fd());
  }

  recv_buffer_.clear();
  recv_buffer_.ShrinkToFit();

  write_buffer_.clear();
  write_buffer_.ShrinkToFit();

  write_registered_ = false;
}

void TcpChannel::TryFlushWriteBuffer(std::size_t write_budget) {
  std::size_t flushes = 0;
  while (write_buffer_.ReadableSize() > 0) {
    if (flushes >= write_budget) {
      UpdateWriteInterest();
      CancelWriteBufferShrink();
      return;
    }

    auto rspan = write_buffer_.ReadableSpan();
    auto result = socket_.Send(rspan);
    if (!result) {
      if (result.Error().Code() == ErrorCode::kWouldBlock) {
        UpdateWriteInterest();
        return;
      }
      ATLAS_LOG_WARNING("TcpChannel send error to {}: {}", remote_.ToString(),
                        result.Error().Message());
      OnDisconnect();
      return;
    }

    write_buffer_.Consume(*result);
    ++flushes;
  }

  if (write_registered_) {
    UpdateWriteInterest();
  }

  if (write_buffer_.ReadableSize() == 0) {
    ScheduleWriteBufferShrink();
  } else {
    CancelWriteBufferShrink();
  }
}

void TcpChannel::UpdateWriteInterest() {
  bool need_write = write_buffer_.ReadableSize() > 0;
  if (need_write && !write_registered_) {
    auto interest = IOEvent::kReadable | IOEvent::kWritable;
    auto result = dispatcher_.ModifyInterest(socket_.Fd(), interest);
    if (!result) {
      ATLAS_LOG_ERROR("Failed to set write interest for {}: {}", remote_.ToString(),
                      result.Error().Message());
      OnDisconnect();
      return;
    }
    write_registered_ = true;
  } else if (!need_write && write_registered_) {
    auto result = dispatcher_.ModifyInterest(socket_.Fd(), IOEvent::kReadable);
    if (!result) {
      ATLAS_LOG_ERROR("Failed to clear write interest for {}: {}", remote_.ToString(),
                      result.Error().Message());
      OnDisconnect();
      return;
    }
    write_registered_ = false;
  }
}

void TcpChannel::ScheduleRecvBufferShrink() {
  if (recv_buffer_.Capacity() <= recv_buffer_.MinCapacity() ||
      recv_buffer_shrink_timer_.IsValid()) {
    return;
  }

  recv_buffer_shrink_timer_ = dispatcher_.AddTimer(
      kBufferShrinkDelay, [this](TimerHandle handle) { OnRecvBufferShrink(handle); });
}

void TcpChannel::CancelRecvBufferShrink() {
  if (recv_buffer_shrink_timer_.IsValid()) {
    dispatcher_.CancelTimer(recv_buffer_shrink_timer_);
    recv_buffer_shrink_timer_ = TimerHandle{};
  }
}

void TcpChannel::OnRecvBufferShrink(TimerHandle handle) {
  if (recv_buffer_shrink_timer_ != handle) {
    return;
  }

  recv_buffer_shrink_timer_ = TimerHandle{};
  if (recv_buffer_.ReadableSize() == 0) {
    recv_buffer_.ShrinkToFit();
  }
}

void TcpChannel::ScheduleWriteBufferShrink() {
  if (write_buffer_.Capacity() <= write_buffer_.MinCapacity() ||
      write_buffer_shrink_timer_.IsValid()) {
    return;
  }

  write_buffer_shrink_timer_ = dispatcher_.AddTimer(
      kBufferShrinkDelay, [this](TimerHandle handle) { OnWriteBufferShrink(handle); });
}

void TcpChannel::CancelWriteBufferShrink() {
  if (write_buffer_shrink_timer_.IsValid()) {
    dispatcher_.CancelTimer(write_buffer_shrink_timer_);
    write_buffer_shrink_timer_ = TimerHandle{};
  }
}

void TcpChannel::OnWriteBufferShrink(TimerHandle handle) {
  if (write_buffer_shrink_timer_ != handle) {
    return;
  }

  write_buffer_shrink_timer_ = TimerHandle{};
  if (write_buffer_.ReadableSize() == 0) {
    write_buffer_.ShrinkToFit();
  }
}

}  // namespace atlas
