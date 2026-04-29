#ifndef ATLAS_LIB_NETWORK_TCP_CHANNEL_H_
#define ATLAS_LIB_NETWORK_TCP_CHANNEL_H_

#include <optional>

#include "foundation/containers/byte_ring_buffer.h"
#include "network/bundle.h"
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

  // Drain the deferred bundle (kBatched messages) into a single TCP
  // frame.  TCP is always reliable, so one bundle suffices (no
  // unreliable side).  Idempotent — empty bundle short-circuits.
  [[nodiscard]] auto FlushDeferred() -> Result<void> override;

 protected:
  [[nodiscard]] auto DoSend(std::span<const std::byte> data) -> Result<size_t> override;
  void OnCondemned() override;

  // Channel batching hooks.  Returns the single deferred bundle (TCP
  // has no reliability axis to split on); OnDeferredAppend marks the
  // channel dirty and triggers an inline flush once the bundle would
  // approach the TCP write_buffer_ cap.
  [[nodiscard]] auto DeferredBundleFor(const MessageDesc& desc) -> class Bundle* override;
  [[nodiscard]] auto OnDeferredAppend(const MessageDesc& desc, std::size_t total_size)
      -> Result<void> override;

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

  // Deferred bundle for kBatched messages.  Single bundle (TCP is
  // always reliable, no unreliable axis).  Drained by FlushDeferred.
  // Threshold sits well under kMaxWriteBufferSize so an inline flush
  // never overflows the ring buffer; it's an OOM safety net like
  // RUDP's threshold, not a per-flush boundary.
  class Bundle deferred_bundle_;
  static constexpr std::size_t kDeferredFlushThreshold = 192 * 1024;
};

}  // namespace atlas

#endif  // ATLAS_LIB_NETWORK_TCP_CHANNEL_H_
