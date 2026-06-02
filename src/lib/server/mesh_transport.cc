#include "server/mesh_transport.h"

#include <array>
#include <cstddef>
#include <span>

#include "network/event_dispatcher.h"
#include "serialization/binary_stream.h"

namespace atlas {

namespace {
// HELLOs are tiny and low-rate; cap the per-callback drain anyway so a flood of
// datagrams cannot starve dispatcher timers.
constexpr int kMaxDatagramsPerCallback = 256;
}  // namespace

MeshTransport::MeshTransport(EventDispatcher& dispatcher) : dispatcher_(dispatcher) {}

MeshTransport::~MeshTransport() { Close(); }

auto MeshTransport::Open(const Address& bind_addr, const Address& broadcast_addr) -> Result<void> {
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

auto MeshTransport::BroadcastHello(const machined::MeshHello& hello) -> Result<std::size_t> {
  return SendHelloTo(broadcast_addr_, hello);
}

auto MeshTransport::SendHelloTo(const Address& dest, const machined::MeshHello& hello)
    -> Result<std::size_t> {
  if (!socket_) return Error{ErrorCode::kInvalidArgument, "MeshTransport not open"};
  BinaryWriter w;
  hello.Serialize(w);
  return socket_->SendTo(w.Data(), dest);
}

void MeshTransport::OnReadable() {
  if (!socket_) return;
  std::array<std::byte, 2048> buf{};
  for (int i = 0; i < kMaxDatagramsPerCallback; ++i) {
    auto recv = socket_->RecvFrom(buf);
    if (!recv) break;  // kWouldBlock or error ends the drain
    auto [bytes, src] = *recv;
    BinaryReader r(std::span<const std::byte>(buf.data(), bytes));
    auto type = machined::PeekMeshType(r);
    if (!type) continue;
    if (*type == machined::MeshMessageType::kHello) {
      auto hello = machined::MeshHello::Deserialize(r);
      if (hello && hello_cb_) hello_cb_(src, *hello);
    }
  }
}

}  // namespace atlas
