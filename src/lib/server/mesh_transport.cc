#include "server/mesh_transport.h"

#include <cstddef>
#include <span>

#include "network/event_dispatcher.h"

namespace atlas {

namespace {
// Mesh datagrams are low-rate; cap the per-callback drain so a flood cannot
// starve dispatcher timers.
constexpr int kMaxDatagramsPerCallback = 256;
}  // namespace

MeshTransport::MeshTransport(EventDispatcher& dispatcher) : dispatcher_(dispatcher) {}

MeshTransport::~MeshTransport() { Close(); }

auto MeshTransport::Open(const Address& bind_addr, const Address& broadcast_addr) -> Result<void> {
  Close();  // Drop any prior socket + reader so a re-Open never leaks the fd.
  auto sock = Socket::CreateUdp();
  if (!sock) return sock.Error();
  if (auto r = sock->SetReuseAddr(true); !r) return r.Error();
  if (auto r = sock->SetBroadcast(true); !r) return r.Error();
  if (auto r = sock->SetNonBlocking(true); !r) return r.Error();
  if (auto r = sock->Bind(bind_addr); !r) return r.Error();

  auto local = sock->LocalAddress();
  if (!local) return local.Error();
  local_addr_ = *local;
  broadcast_addr_ = broadcast_addr;
  socket_ = std::move(*sock);

  auto reg = dispatcher_.RegisterReader(socket_->Fd(), [this](FdHandle, IOEvent) { OnReadable(); });
  if (!reg) {
    socket_.reset();
    return reg.Error();
  }
  return Result<void>{};
}

void MeshTransport::Close() {
  if (!socket_) return;
  (void)dispatcher_.Deregister(socket_->Fd());
  socket_.reset();
}

auto MeshTransport::Broadcast(std::span<const std::byte> payload) -> Result<std::size_t> {
  return SendTo(broadcast_addr_, payload);
}

auto MeshTransport::SendTo(const Address& dest, std::span<const std::byte> payload)
    -> Result<std::size_t> {
  if (!socket_) return Error{ErrorCode::kInvalidArgument, "MeshTransport not open"};
  return socket_->SendTo(payload, dest);
}

void MeshTransport::OnReadable() {
  if (!socket_) return;
  for (int i = 0; i < kMaxDatagramsPerCallback; ++i) {
    auto recv = socket_->RecvFrom(recv_buf_);
    if (!recv) break;  // kWouldBlock or error ends the drain
    auto [bytes, src] = *recv;
    if (datagram_cb_) datagram_cb_(src, std::span<const std::byte>(recv_buf_.data(), bytes));
  }
}

}  // namespace atlas
