#ifndef ATLAS_LIB_NETWORK_TCP_CHANNEL_H_
#define ATLAS_LIB_NETWORK_TCP_CHANNEL_H_

#include <optional>

#include "foundation/containers/byte_ring_buffer.h"
#include "network/channel.h"
#include "network/socket.h"

namespace atlas {

class TcpChannel : public Channel {
 public:
  static constexpr std::size_t kFrameHeaderSize = sizeof(uint32_t);
  static constexpr std::size_t kInitialRecvBufferSize = 16 * 1024;
  static constexpr std::size_t kMaxRecvBufferSize = 1024 * 1024;
  static constexpr std::size_t kInitialWriteBufferSize = 16 * 1024;
  static constexpr std::size_t kMaxWriteBufferSize = 256 * 1024;
  static constexpr Duration kBufferShrinkDelay = std::chrono::seconds(30);
  static constexpr std::size_t kMaxSocketReadsPerCallback = 64;
  static constexpr std::size_t kMaxWriteFlushesPerCallback = 64;

  TcpChannel(EventDispatcher& dispatcher, InterfaceTable& table, Socket socket,
             const Address& remote);
  ~TcpChannel() override;

  [[nodiscard]] auto Fd() const -> FdHandle override { return socket_.Fd(); }
  [[nodiscard]] auto RecvBufferCapacity() const -> std::size_t { return recv_buffer_.Capacity(); }
  [[nodiscard]] auto RecvBufferSize() const -> std::size_t { return recv_buffer_.ReadableSize(); }
  [[nodiscard]] auto WriteBufferCapacity() const -> std::size_t { return write_buffer_.Capacity(); }
  [[nodiscard]] auto WriteBufferSize() const -> std::size_t { return write_buffer_.ReadableSize(); }

  void OnReadable();
  void OnWritable();

 protected:
  [[nodiscard]] auto DoSend(std::span<const std::byte> data) -> Result<size_t> override;
  void OnCondemned() override;

 private:
  [[nodiscard]] auto EnsureRecvWritable() -> bool;
  [[nodiscard]] auto PeekFrameLength() const -> std::optional<uint32_t>;
  void ProcessRecvBuffer();
  void TryFlushWriteBuffer(std::size_t write_budget = kMaxWriteFlushesPerCallback);
  void UpdateWriteInterest();
  void ScheduleRecvBufferShrink();
  void CancelRecvBufferShrink();
  void OnRecvBufferShrink(TimerHandle handle);
  void ScheduleWriteBufferShrink();
  void CancelWriteBufferShrink();
  void OnWriteBufferShrink(TimerHandle handle);

  Socket socket_;

  ByteRingBuffer recv_buffer_{kInitialRecvBufferSize, kMaxRecvBufferSize};
  ByteRingBuffer write_buffer_{kInitialWriteBufferSize, kMaxWriteBufferSize};
  TimerHandle recv_buffer_shrink_timer_;
  TimerHandle write_buffer_shrink_timer_;

  bool write_registered_{false};
};

}  // namespace atlas

#endif  // ATLAS_LIB_NETWORK_TCP_CHANNEL_H_
