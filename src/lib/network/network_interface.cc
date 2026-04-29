#include "network/network_interface.h"

#include <algorithm>

#include "foundation/log.h"
#include "foundation/profiler.h"
#include "network/event_dispatcher.h"
#include "network/reliable_udp.h"
#include "network/tcp_channel.h"
#include "network/udp_channel.h"

namespace atlas {

namespace {

void ApplyRudpProfile(ReliableUdpChannel& channel, const NetworkInterface::RudpProfile& profile) {
  channel.SetNocwnd(profile.nocwnd);
  channel.SetSendWindow(profile.send_window);
  channel.SetRecvWindow(profile.recv_window);
  channel.SetMtu(profile.mtu);
  channel.SetDeferredFlushThreshold(profile.deferred_flush_threshold);
}

}  // namespace

// ============================================================================
// Constructor / Destructor
// ============================================================================

NetworkInterface::NetworkInterface(EventDispatcher& dispatcher)
    : dispatcher_(dispatcher), registration_(dispatcher_.AddFrequentTask(this)) {}

NetworkInterface::~NetworkInterface() {
  // registration_ destructor automatically calls dispatcher_.remove_frequent_task(this).
  // Explicitly reset here to ensure removal happens before channel teardown.
  registration_.Reset();

  channels_.clear();
  channels_by_id_.clear();
  condemned_.clear();

  if (tcp_listen_socket_) {
    (void)dispatcher_.Deregister(tcp_listen_socket_->Fd());
    tcp_listen_socket_->Close();
    tcp_listen_socket_.reset();
  }

  if (udp_socket_) {
    (void)dispatcher_.Deregister(udp_socket_->Fd());
    udp_socket_->Close();
    udp_socket_.reset();
  }

  if (rudp_socket_) {
    (void)dispatcher_.Deregister(rudp_socket_->Fd());
    rudp_socket_->Close();
    rudp_socket_.reset();
  }
}

// ============================================================================
// TCP server
// ============================================================================

auto NetworkInterface::StartTcpServer(const Address& addr) -> Result<void> {
  // Close existing listen socket if called more than once
  if (tcp_listen_socket_) {
    (void)dispatcher_.Deregister(tcp_listen_socket_->Fd());
    tcp_listen_socket_->Close();
    tcp_listen_socket_.reset();
  }

  auto sock = Socket::CreateTcp();
  if (!sock) {
    return sock.Error();
  }

  if (auto r = sock->SetReuseAddr(true); !r) return r.Error();
  if (auto r = sock->SetNonBlocking(true); !r) return r.Error();

  auto bind_result = sock->Bind(addr);
  if (!bind_result) {
    return bind_result.Error();
  }

  auto listen_result = sock->Listen();
  if (!listen_result) {
    return listen_result.Error();
  }

  auto local = sock->LocalAddress();
  if (!local) {
    return local.Error();
  }
  tcp_address_ = *local;

  tcp_listen_socket_ = std::move(*sock);

  // Register for accept events
  auto reg = dispatcher_.RegisterReader(tcp_listen_socket_->Fd(),
                                        [this](FdHandle, IOEvent) { OnTcpAccept(); });
  if (!reg) {
    return reg.Error();
  }

  ATLAS_LOG_INFO("TCP server listening on {}", tcp_address_.ToString());
  return Result<void>{};
}

// ============================================================================
// TCP client (async connect)
// ============================================================================

auto NetworkInterface::ConnectTcp(const Address& addr) -> Result<TcpChannel*> {
  if (shutting_down_) {
    return Error(ErrorCode::kChannelCondemned, "Shutting down");
  }

  auto sock = Socket::CreateTcp();
  if (!sock) {
    return sock.Error();
  }

  if (auto r = sock->SetNonBlocking(true); !r) return r.Error();
  if (auto r = sock->SetNoDelay(true); !r) return r.Error();

  auto conn = sock->Connect(addr);
  if (!conn && conn.Error().Code() != ErrorCode::kWouldBlock) {
    return conn.Error();
  }

  auto channel =
      std::make_unique<TcpChannel>(dispatcher_, interface_table_, std::move(*sock), addr);
  channel->SetChannelId(next_channel_id_++);

  auto reg = dispatcher_.RegisterReader(channel->Fd(),
                                        [this, ch = channel.get()](FdHandle, IOEvent events) {
                                          if ((events & IOEvent::kReadable) != IOEvent::kNone) {
                                            ATLAS_PROFILE_ZONE_N("TcpChannel::OnReadable");
                                            ch->OnReadable();
                                          }
                                          if ((events & IOEvent::kWritable) != IOEvent::kNone) {
                                            ATLAS_PROFILE_ZONE_N("TcpChannel::OnWritable");
                                            ch->OnWritable();
                                          }
                                          // Flush any deferred sends staged by handlers run during
                                          // OnReadable.  Same role as the FlushDirtySendChannels
                                          // call at the end of OnRudpReadable: end-of-handler-batch
                                          // is the canonical batching window for inbound-driven
                                          // sends.
                                          FlushDirtySendChannels();
                                        });
  if (!reg) {
    return reg.Error();
  }

  channel->SetDisconnectCallback([this](Channel& ch) { OnChannelDisconnect(ch); });
  channel->SetMarkDirtyCallback([this](Channel& ch) { MarkChannelDirty(ch); });
  channel->Activate();

  auto* raw = channel.get();
  channels_by_id_[raw->ChannelId()] = raw;
  channels_[addr] = std::move(channel);
  return raw;
}

// ============================================================================
// UDP endpoint
// ============================================================================

auto NetworkInterface::StartUdp(const Address& addr) -> Result<void> {
  auto sock = Socket::CreateUdp();
  if (!sock) {
    return sock.Error();
  }

  if (auto r = sock->SetReuseAddr(true); !r) return r.Error();
  if (auto r = sock->SetNonBlocking(true); !r) return r.Error();

  auto bind_result = sock->Bind(addr);
  if (!bind_result) {
    return bind_result.Error();
  }

  auto local = sock->LocalAddress();
  if (!local) {
    return local.Error();
  }
  udp_address_ = *local;

  udp_socket_ = std::move(*sock);

  auto reg =
      dispatcher_.RegisterReader(udp_socket_->Fd(), [this](FdHandle, IOEvent) { OnUdpReadable(); });
  if (!reg) {
    return reg.Error();
  }

  ATLAS_LOG_INFO("UDP listening on {}", udp_address_.ToString());
  return Result<void>{};
}

auto NetworkInterface::ConnectUdp(const Address& addr) -> Result<UdpChannel*> {
  if (shutting_down_) return Error(ErrorCode::kChannelCondemned, "Shutting down");

  // Open shared UDP socket on first use (bind to any port)
  if (!udp_socket_) {
    if (auto r = StartUdp(Address(0, 0)); !r) return r.Error();
  }

  // Re-use existing channel if already targeting this peer
  if (auto it = channels_.find(addr); it != channels_.end()) {
    auto* udp = dynamic_cast<UdpChannel*>(it->second.get());
    if (!udp) return Error(ErrorCode::kAlreadyExists, "Channel exists with different protocol");
    return udp;
  }

  auto channel = std::make_unique<UdpChannel>(dispatcher_, interface_table_, *udp_socket_, addr);
  channel->SetChannelId(next_channel_id_++);
  channel->SetDisconnectCallback([this](Channel& ch) { OnChannelDisconnect(ch); });
  channel->SetMarkDirtyCallback([this](Channel& ch) { MarkChannelDirty(ch); });
  channel->Activate();

  auto* raw = static_cast<UdpChannel*>(channel.get());
  channels_by_id_[raw->ChannelId()] = raw;
  channels_[addr] = std::move(channel);
  return raw;
}

// ============================================================================
// Channel access
// ============================================================================

auto NetworkInterface::FindChannel(const Address& addr) -> Channel* {
  auto it = channels_.find(addr);
  if (it != channels_.end()) {
    return it->second.get();
  }
  return nullptr;
}

auto NetworkInterface::FindChannel(ChannelId id) -> Channel* {
  auto it = channels_by_id_.find(id);
  if (it != channels_by_id_.end()) {
    return it->second;
  }
  return nullptr;
}

auto NetworkInterface::ChannelCount() const -> size_t {
  return channels_.size();
}

// ============================================================================
// Addresses
// ============================================================================

auto NetworkInterface::TcpAddress() const -> Address {
  return tcp_address_;
}

auto NetworkInterface::UdpAddress() const -> Address {
  return udp_address_;
}

auto NetworkInterface::RudpAddress() const -> Address {
  return rudp_address_;
}

// ============================================================================
// RUDP server / client
// ============================================================================

auto NetworkInterface::StartRudpServer(const Address& addr) -> Result<void> {
  return StartRudpServer(addr, RudpProfile{});
}

auto NetworkInterface::StartRudpServer(const Address& addr, const RudpProfile& accept_profile)
    -> Result<void> {
  if (rudp_socket_) {
    return Error(ErrorCode::kAlreadyExists, "RUDP socket already open");
  }

  auto sock = Socket::CreateUdp();
  if (!sock) return sock.Error();

  if (auto r = sock->SetReuseAddr(true); !r) return r.Error();
  if (auto r = sock->SetNonBlocking(true); !r) return r.Error();
  if (auto r = sock->SetRecvBufferSize(4 * 1024 * 1024); !r)
    ATLAS_LOG_WARNING("RUDP: failed to set recv buffer size: {}", r.Error().Message());
  if (auto r = sock->SetSendBufferSize(4 * 1024 * 1024); !r)
    ATLAS_LOG_WARNING("RUDP: failed to set send buffer size: {}", r.Error().Message());

  if (auto r = sock->Bind(addr); !r) return r.Error();

  auto local = sock->LocalAddress();
  if (!local) return local.Error();
  rudp_address_ = *local;

  rudp_socket_ = std::move(*sock);
  rudp_server_mode_ = true;
  rudp_accept_profile_ = accept_profile;

  auto reg = dispatcher_.RegisterReader(rudp_socket_->Fd(),
                                        [this](FdHandle, IOEvent) { OnRudpReadable(); });
  if (!reg) return reg.Error();

  ATLAS_LOG_INFO("RUDP server listening on {}", rudp_address_.ToString());
  return Result<void>{};
}

auto NetworkInterface::ConnectRudp(const Address& addr) -> Result<ReliableUdpChannel*> {
  return ConnectRudp(addr, RudpProfile{});
}

auto NetworkInterface::ConnectRudp(const Address& addr, const RudpProfile& profile)
    -> Result<ReliableUdpChannel*> {
  if (shutting_down_) return Error(ErrorCode::kChannelCondemned, "Shutting down");

  // Open shared RUDP socket on first use (client side: bind to any port)
  if (!rudp_socket_) {
    auto sock = Socket::CreateUdp();
    if (!sock) return sock.Error();

    if (auto r = sock->SetNonBlocking(true); !r) return r.Error();
    if (auto r = sock->SetRecvBufferSize(4 * 1024 * 1024); !r)
      ATLAS_LOG_WARNING("RUDP: failed to set recv buffer size: {}", r.Error().Message());
    if (auto r = sock->SetSendBufferSize(4 * 1024 * 1024); !r)
      ATLAS_LOG_WARNING("RUDP: failed to set send buffer size: {}", r.Error().Message());

    const Address kBindAddr = rudp_client_bind_address_.value_or(Address(0, 0));
    if (auto r = sock->Bind(kBindAddr); !r) return r.Error();

    auto local = sock->LocalAddress();
    if (!local) return local.Error();
    rudp_address_ = *local;

    rudp_socket_ = std::move(*sock);

    auto reg = dispatcher_.RegisterReader(rudp_socket_->Fd(),
                                          [this](FdHandle, IOEvent) { OnRudpReadable(); });
    if (!reg) return reg.Error();
  }

  // Re-use existing channel if already connected to this peer
  if (auto it = channels_.find(addr); it != channels_.end()) {
    auto* rudp = dynamic_cast<ReliableUdpChannel*>(it->second.get());
    if (!rudp) return Error(ErrorCode::kAlreadyExists, "Channel exists with different protocol");
    return rudp;
  }

  auto channel =
      std::make_unique<ReliableUdpChannel>(dispatcher_, interface_table_, *rudp_socket_, addr);
  channel->SetChannelId(next_channel_id_++);
  channel->SetDisconnectCallback([this](Channel& ch) { OnChannelDisconnect(ch); });
  channel->SetMarkDirtyCallback([this](Channel& ch) { MarkChannelDirty(ch); });
  ApplyRudpProfile(*channel, profile);
  channel->Activate();

  auto* raw = static_cast<ReliableUdpChannel*>(channel.get());
  channels_by_id_[raw->ChannelId()] = raw;
  channels_[addr] = std::move(channel);
  return raw;
}

auto NetworkInterface::InternetRudpProfile() -> RudpProfile {
  // ET-style outer profile.  MTU = 470 is calibrated against consumer
  // ISP / VPN / mobile-carrier paths (PPPoE 1492, IPSec/WireGuard
  // ~50 B overhead, mobile MSS sometimes ~1280) — well below KCP's
  // 1400 default.  Window stays at 256 to bound retransmit memory on
  // a wide-area lossy link.
  //
  // Deferred flush threshold acts as an OOM safety net only: at
  // ~32 KB the bundle is large enough that tick-end flush amortises
  // the Finalize / filter / packet-building cost across many
  // fragments in SendFragmented's tight loop.  Earlier mid-tick
  // flushes (e.g. at MTU boundary) regress per-call overhead
  // 3-4× under high-volume pumps — see 500-cli baseline 4057994
  // vs b1782e5 in docs/optimization/.
  RudpProfile profile;
  profile.mtu = 470;
  profile.deferred_flush_threshold = 32 * 1024;
  return profile;
}

auto NetworkInterface::ClusterRudpProfile() -> RudpProfile {
  // Intra-DC profile.  cwnd disabled (no fairness pressure on a
  // dedicated link), large window for high throughput, MTU at the
  // KCP default (1400) which fits comfortably on 1500-byte Ethernet.
  // Deferred flush threshold sits just below kMaxBundleSize (64 KB)
  // so it only fires as an OOM safety net — tick-end flush is the
  // primary drain.  See InternetRudpProfile for rationale.
  RudpProfile profile;
  profile.nocwnd = true;
  profile.send_window = 4096;
  profile.recv_window = 4096;
  profile.mtu = 1400;
  profile.deferred_flush_threshold = 60 * 1024;
  return profile;
}

void NetworkInterface::SetRudpClientBindAddress(const Address& addr) {
  rudp_client_bind_address_ = addr;
}

auto NetworkInterface::ConnectRudpNocwnd(const Address& addr) -> Result<ReliableUdpChannel*> {
  // Re-use existing channel without changing its nocwnd flag
  if (auto it = channels_.find(addr); it != channels_.end()) {
    auto* rudp = dynamic_cast<ReliableUdpChannel*>(it->second.get());
    if (!rudp) return Error(ErrorCode::kAlreadyExists, "Channel exists with different protocol");
    return rudp;
  }

  auto result = ConnectRudp(addr, ClusterRudpProfile());
  if (!result) return result;
  return result;
}

// ============================================================================
// Rate limiting
// ============================================================================

void NetworkInterface::SetRateLimit(uint32_t max_per_second) {
  rate_limit_ = max_per_second;
}

void NetworkInterface::SetAcceptCallback(AcceptCallback cb) {
  accept_callback_ = std::move(cb);
}

void NetworkInterface::SetDisconnectCallback(DisconnectCallback cb) {
  disconnect_callback_ = std::move(cb);
}

// ============================================================================
// Shutdown
// ============================================================================

void NetworkInterface::PrepareForShutdown() {
  shutting_down_ = true;

  while (!channels_.empty()) {
    CondemnChannel(channels_.begin()->first);
  }

  ATLAS_LOG_INFO("NetworkInterface preparing for shutdown, {} channels condemned",
                 condemned_.size());
}

// ============================================================================
// FrequentTask
// ============================================================================

void NetworkInterface::DoTask() {
  ATLAS_PROFILE_ZONE_N("NetworkInterface::DoTask");
  ProcessCondemnedChannels();

  // Tick-cadence backup for hot channels.  OnRudpReadable normally
  // re-flushes them within the same callback budget, but if a channel
  // yielded and no fresh datagram arrived to retrigger
  // OnRudpReadable, the backlog would otherwise stall until the
  // peer's next packet.  Bound the per-DoTask drain to half the
  // OnRudpReadable budget so we don't replicate the cascade-stall
  // problem we set out to fix.
  if (!hot_channels_.empty()) {
    DrainHotChannels(Clock::now() + kReadableCallbackBudget / 2);
  }

  if (rate_limit_ > 0) {
    auto now = Clock::now();
    if (now - last_rate_cleanup_ >= kRateCleanupInterval) {
      CleanupStaleRateTrackers();
      last_rate_cleanup_ = now;
    }
  }
}

// ============================================================================
// IO callbacks
// ============================================================================

void NetworkInterface::OnTcpAccept() {
  ATLAS_PROFILE_ZONE_N("NetworkInterface::OnTcpAccept");
  std::size_t accepts = 0;
  while (true) {
    if (CallbackBudgetExhausted(accepts, kMaxAcceptsPerCallback)) {
      break;
    }

    auto result = tcp_listen_socket_->Accept();
    if (!result) {
      if (result.Error().Code() == ErrorCode::kWouldBlock) {
        break;
      }
      ATLAS_LOG_WARNING("TCP accept error: {}", result.Error().Message());
      break;
    }

    auto& [peer_sock, peer_addr] = *result;
    ++accepts;

    // Rate check
    if (rate_limit_ > 0 && !CheckRateLimit(peer_addr.Ip())) {
      ATLAS_LOG_WARNING("Rate limited connection from {}", peer_addr.ToString());
      continue;  // Socket destructor closes it
    }

    if (shutting_down_) {
      continue;
    }

    if (auto nd = peer_sock.SetNoDelay(true); !nd) {
      ATLAS_LOG_WARNING("Failed to set TCP_NODELAY on accepted connection from {}: {}",
                        peer_addr.ToString(), nd.Error().Message());
    }

    auto channel = std::make_unique<TcpChannel>(dispatcher_, interface_table_, std::move(peer_sock),
                                                peer_addr);
    channel->SetChannelId(next_channel_id_++);

    auto reg = dispatcher_.RegisterReader(channel->Fd(),
                                          [this, ch = channel.get()](FdHandle, IOEvent events) {
                                            if ((events & IOEvent::kReadable) != IOEvent::kNone) {
                                              ATLAS_PROFILE_ZONE_N("TcpChannel::OnReadable");
                                              ch->OnReadable();
                                            }
                                            if ((events & IOEvent::kWritable) != IOEvent::kNone) {
                                              ATLAS_PROFILE_ZONE_N("TcpChannel::OnWritable");
                                              ch->OnWritable();
                                            }
                                            FlushDirtySendChannels();
                                          });
    if (!reg) {
      ATLAS_LOG_ERROR("Failed to register channel fd: {}", reg.Error().Message());
      continue;
    }

    channel->SetDisconnectCallback([this](Channel& ch) { OnChannelDisconnect(ch); });
    channel->SetMarkDirtyCallback([this](Channel& ch) { MarkChannelDirty(ch); });
    channel->Activate();

    ATLAS_LOG_DEBUG("Accepted TCP connection from {}", peer_addr.ToString());
    channels_by_id_[channel->ChannelId()] = channel.get();
    channels_[peer_addr] = std::move(channel);
    if (accept_callback_) {
      accept_callback_(*channels_[peer_addr]);
    }
  }
}

void NetworkInterface::OnUdpReadable() {
  ATLAS_PROFILE_ZONE_N("NetworkInterface::OnUdpReadable");
  const auto deadline = Clock::now() + kReadableCallbackBudget;
  auto recv_buffer = DatagramRecvBuffer();
  std::size_t datagrams = 0;
  while (datagrams < kMaxDatagramsPerCallback) {
    if (Clock::now() >= deadline) break;

    auto result = udp_socket_->RecvFrom(recv_buffer);
    if (!result) {
      if (result.Error().Code() == ErrorCode::kWouldBlock) {
        break;
      }
      if (result.Error().Code() == ErrorCode::kConnectionReset) {
        ++datagrams;
        continue;
      }
      ATLAS_LOG_WARNING("UDP recv error: {}", result.Error().Message());
      break;
    }

    auto [bytes, src_addr] = *result;
    ++datagrams;
    if (bytes == 0) {
      continue;
    }

    // Rate check
    if (rate_limit_ > 0 && !CheckRateLimit(src_addr.Ip())) {
      continue;
    }

    // Find or create UDP channel for this peer
    auto it = channels_.find(src_addr);
    if (it == channels_.end()) {
      if (shutting_down_ || channels_.size() >= kMaxChannels) {
        continue;
      }

      auto channel =
          std::make_unique<UdpChannel>(dispatcher_, interface_table_, *udp_socket_, src_addr);
      channel->SetChannelId(next_channel_id_++);
      channel->SetDisconnectCallback([this](Channel& ch) { OnChannelDisconnect(ch); });
      channel->SetMarkDirtyCallback([this](Channel& ch) { MarkChannelDirty(ch); });
      channel->Activate();
      channels_by_id_[channel->ChannelId()] = channel.get();
      auto [inserted_it, _] = channels_.emplace(src_addr, std::move(channel));
      it = inserted_it;
    }

    auto* udp_ch = static_cast<UdpChannel*>(it->second.get());
    udp_ch->OnDatagramReceived(recv_buffer.first(bytes));
  }

  // Flush any deferred sends staged by the handlers we just dispatched.
  // For UdpChannel this is a no-op (TCP/UDP fall-through path doesn't
  // populate dirty_channels_), kept for symmetry with the RUDP path.
  FlushDirtySendChannels();
}

void NetworkInterface::OnRudpReadable() {
  ATLAS_PROFILE_ZONE_N("NetworkInterface::OnRudpReadable");
  const auto deadline = Clock::now() + kReadableCallbackBudget;
  auto recv_buffer = DatagramRecvBuffer();
  std::size_t datagrams = 0;
  while (datagrams < kMaxDatagramsPerCallback) {
    if (Clock::now() >= deadline) break;

    auto result = rudp_socket_->RecvFrom(recv_buffer);
    if (!result) {
      if (result.Error().Code() == ErrorCode::kWouldBlock) break;
      if (result.Error().Code() == ErrorCode::kConnectionReset) {
        ++datagrams;
        continue;
      }
      ATLAS_LOG_WARNING("RUDP recv error: {}", result.Error().Message());
      break;
    }

    auto [bytes, src_addr] = *result;
    ++datagrams;
    if (bytes == 0) continue;

    if (rate_limit_ > 0 && !CheckRateLimit(src_addr.Ip())) continue;

    auto it = channels_.find(src_addr);
    if (it == channels_.end()) {
      if (!rudp_server_mode_ || shutting_down_ || channels_.size() >= kMaxChannels) continue;

      auto channel = std::make_unique<ReliableUdpChannel>(dispatcher_, interface_table_,
                                                          *rudp_socket_, src_addr);
      channel->SetChannelId(next_channel_id_++);
      channel->SetDisconnectCallback([this](Channel& ch) { OnChannelDisconnect(ch); });
      channel->SetMarkDirtyCallback([this](Channel& ch) { MarkChannelDirty(ch); });
      channel->SetHotCallback([this](ReliableUdpChannel& ch) { hot_channels_.insert(&ch); });
      ApplyRudpProfile(*channel, rudp_accept_profile_);
      channel->Activate();

      ATLAS_LOG_DEBUG("RUDP: new peer {}", src_addr.ToString());
      channels_by_id_[channel->ChannelId()] = channel.get();
      auto [inserted_it, _] = channels_.emplace(src_addr, std::move(channel));
      it = inserted_it;
      if (accept_callback_) accept_callback_(*it->second);
    }

    auto* rudp_ch = static_cast<ReliableUdpChannel*>(it->second.get());
    rudp_ch->OnDatagramReceived(recv_buffer.first(bytes), deadline);
  }

  // Re-flush channels that yielded mid-cascade.  We share the same
  // 10 ms deadline as the recv loop so a single OnRudpReadable callback
  // is bounded end-to-end; whatever doesn't drain here gets a fresh
  // budget on the next callback or via DoTask's tick-cadence backup.
  if (!hot_channels_.empty()) {
    DrainHotChannels(deadline);
  }

  // Flush deferred sends staged by handlers run during this callback.
  // Dispatch (handler) → SendMessage(kBatched) → BufferMessageDeferred
  // → MarkChannelDirty.  This is the canonical batching window for
  // any send triggered by inbound traffic.
  FlushDirtySendChannels();
}

void NetworkInterface::FlushDirtySendChannels() {
  // Iterate by move-and-clear so a channel that re-dirties itself
  // mid-flush (e.g. a packet_filter chain that calls back into
  // SendMessage with kBatched urgency) lands in a fresh set instead
  // of an iterator we're still walking.
  if (dirty_channels_.empty()) return;
  ATLAS_PROFILE_ZONE_N("NetworkInterface::FlushDirtySendChannels");
  std::unordered_set<Channel*> snapshot;
  snapshot.swap(dirty_channels_);
  for (Channel* ch : snapshot) {
    // Channel::FlushDeferred is virtual; the default no-op covers
    // transports without a deferred bundle (plain UDP).  RUDP and
    // TCP both override.  No dynamic_cast needed.
    if (auto r = ch->FlushDeferred(); !r) {
      ATLAS_LOG_DEBUG("FlushDirty: channel {} returned {}", ch->RemoteAddress().ToString(),
                      r.Error().Message());
    }
  }
}

void NetworkInterface::DrainHotChannels(TimePoint deadline) {
  for (auto it = hot_channels_.begin(); it != hot_channels_.end();) {
    if (Clock::now() >= deadline) break;
    ReliableUdpChannel* ch = *it;
    const bool still_hot = ch->FlushReceiveBuffer(deadline);
    if (!still_hot) {
      it = hot_channels_.erase(it);
    } else {
      ++it;
    }
  }
}

// ============================================================================
// Channel lifecycle
// ============================================================================

void NetworkInterface::OnChannelDisconnect(Channel& channel) {
  // Copy callback data before condemn_channel, which may re-enter
  // on_channel_disconnect if the disconnect_callback_ triggers further
  // disconnects (cascading). Condemning first ensures the channel is
  // removed from the active set before any callback runs.
  auto addr = channel.RemoteAddress();
  CondemnChannel(addr);

  // Invoke after condemn to avoid re-entrancy on the same channel.
  if (disconnect_callback_) {
    disconnect_callback_(channel);
  }
}

void NetworkInterface::CondemnChannel(const Address& addr) {
  auto it = channels_.find(addr);
  if (it == channels_.end()) {
    return;
  }

  auto channel = std::move(it->second);
  channels_.erase(it);
  channels_by_id_.erase(channel->ChannelId());

  // hot_channels_ holds raw pointers; if this channel had pending
  // backlog when it was condemned, scrub the entry so the next
  // DrainHotChannels pass doesn't dereference the moved-out unique_ptr.
  if (auto* rudp = dynamic_cast<ReliableUdpChannel*>(channel.get())) {
    hot_channels_.erase(rudp);
  }
  // dirty_channels_ holds raw Channel* pointers; same scrub rule.
  dirty_channels_.erase(channel.get());

  // UDP and RUDP channels share a single socket — deregistering it would
  // break all other channels on that socket. Only deregister for TCP.
  const bool kSharedFd = (rudp_socket_ && channel->Fd() == rudp_socket_->Fd()) ||
                         (udp_socket_ && channel->Fd() == udp_socket_->Fd());
  if (!kSharedFd) (void)dispatcher_.Deregister(channel->Fd());
  channel->Condemn();

  condemned_.push_back({std::move(channel), Clock::now()});

  // Enforce the cap: force-close the oldest entry if we are over the limit.
  while (condemned_.size() > kMaxCondemnedChannels) {
    ATLAS_LOG_WARNING(
        "NetworkInterface: condemned channel list at capacity ({}), "
        "force-closing oldest entry",
        kMaxCondemnedChannels);
    condemned_.pop_front();
  }
}

void NetworkInterface::ProcessCondemnedChannels() {
  auto now = Clock::now();
  while (!condemned_.empty() && (now - condemned_.front().condemned_at) >= kCondemnTimeout) {
    condemned_.pop_front();
  }
}

auto NetworkInterface::DatagramRecvBuffer() -> std::span<std::byte> {
  if (!datagram_recv_scratch_) {
    datagram_recv_scratch_ = StreamBufferPool::Instance().Acquire(kMaxDatagramSize);
  }

  return {datagram_recv_scratch_.data(), datagram_recv_scratch_.Capacity()};
}

// ============================================================================
// Rate limiting
// ============================================================================

auto NetworkInterface::CheckRateLimit(uint32_t ip) -> bool {
  static constexpr std::size_t kMaxRateTrackers = 100'000;

  if (rate_trackers_.size() >= kMaxRateTrackers && !rate_trackers_.contains(ip)) {
    return false;
  }

  auto now = Clock::now();
  auto& tracker = rate_trackers_[ip];

  if (now - tracker.window_start >= std::chrono::seconds(1)) {
    tracker.count = 0;
    tracker.window_start = now;
  }

  ++tracker.count;
  return tracker.count <= rate_limit_;
}

auto NetworkInterface::CallbackBudgetExhausted(std::size_t processed, std::size_t budget) -> bool {
  return processed >= budget;
}

void NetworkInterface::CleanupStaleRateTrackers() {
  // Remove entries whose window started more than 2 seconds ago (i.e. inactive IPs).
  auto now = Clock::now();
  std::erase_if(rate_trackers_, [now](const auto& kv) {
    return (now - kv.second.window_start) > std::chrono::seconds(2);
  });
}

}  // namespace atlas
