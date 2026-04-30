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

NetworkInterface::NetworkInterface(EventDispatcher& dispatcher)
    : dispatcher_(dispatcher), registration_(dispatcher_.AddFrequentTask(this)) {}

NetworkInterface::~NetworkInterface() {
  registration_.Reset();

  channels_.clear();
  channels_by_id_.clear();
  condemned_rudp_by_addr_.clear();
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

auto NetworkInterface::StartTcpServer(const Address& addr) -> Result<void> {
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

  auto reg = dispatcher_.RegisterReader(tcp_listen_socket_->Fd(),
                                        [this](FdHandle, IOEvent) { OnTcpAccept(); });
  if (!reg) {
    return reg.Error();
  }

  ATLAS_LOG_INFO("TCP server listening on {}", tcp_address_.ToString());
  return Result<void>{};
}

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

  if (!udp_socket_) {
    if (auto r = StartUdp(Address(0, 0)); !r) return r.Error();
  }

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

auto NetworkInterface::TcpAddress() const -> Address {
  return tcp_address_;
}

auto NetworkInterface::UdpAddress() const -> Address {
  return udp_address_;
}

auto NetworkInterface::RudpAddress() const -> Address {
  return rudp_address_;
}

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
  RudpProfile profile;
  profile.mtu = 470;
  profile.deferred_flush_threshold = 32 * 1024;
  return profile;
}

auto NetworkInterface::ClusterRudpProfile() -> RudpProfile {
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
  if (auto it = channels_.find(addr); it != channels_.end()) {
    auto* rudp = dynamic_cast<ReliableUdpChannel*>(it->second.get());
    if (!rudp) return Error(ErrorCode::kAlreadyExists, "Channel exists with different protocol");
    return rudp;
  }

  auto result = ConnectRudp(addr, ClusterRudpProfile());
  if (!result) return result;
  return result;
}

void NetworkInterface::SetRateLimit(uint32_t max_per_second) {
  rate_limit_ = max_per_second;
}

void NetworkInterface::SetAcceptCallback(AcceptCallback cb) {
  accept_callback_ = std::move(cb);
}

void NetworkInterface::SetDisconnectCallback(DisconnectCallback cb) {
  disconnect_callback_ = std::move(cb);
}

void NetworkInterface::PrepareForShutdown() {
  shutting_down_ = true;

  while (!channels_.empty()) {
    CondemnChannel(channels_.begin()->first);
  }

  ATLAS_LOG_INFO("NetworkInterface preparing for shutdown, {} channels condemned",
                 condemned_.size());
}

void NetworkInterface::DoTask() {
  ATLAS_PROFILE_ZONE_N("NetworkInterface::DoTask");
  ProcessCondemnedChannels();

  // Backup drain so yielded RUDP channels don't wait for the next datagram.
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

    if (rate_limit_ > 0 && !CheckRateLimit(peer_addr.Ip())) {
      ATLAS_LOG_WARNING("Rate limited connection from {}", peer_addr.ToString());
      continue;
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

    if (rate_limit_ > 0 && !CheckRateLimit(src_addr.Ip())) {
      continue;
    }

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
      if (auto cond_it = condemned_rudp_by_addr_.find(src_addr);
          cond_it != condemned_rudp_by_addr_.end()) {
        cond_it->second->OnDatagramReceived(recv_buffer.first(bytes), deadline);
        continue;
      }

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

  if (!hot_channels_.empty()) {
    DrainHotChannels(deadline);
  }

  FlushDirtySendChannels();
}

void NetworkInterface::FlushDirtySendChannels() {
  // Move-and-clear so a channel that re-dirties itself mid-flush lands
  // in a fresh set instead of the iterator we're still walking.
  if (dirty_channels_.empty()) return;
  ATLAS_PROFILE_ZONE_N("NetworkInterface::FlushDirtySendChannels");
  std::unordered_set<Channel*> snapshot;
  snapshot.swap(dirty_channels_);
  for (Channel* ch : snapshot) {
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

void NetworkInterface::OnChannelDisconnect(Channel& channel) {
  auto addr = channel.RemoteAddress();
  CondemnChannel(addr);

  if (disconnect_callback_) {
    disconnect_callback_(channel);
  }
}

void NetworkInterface::CondemnChannel(Address addr) {
  auto it = channels_.find(addr);
  if (it == channels_.end()) {
    return;
  }

  auto channel = std::move(it->second);
  channels_.erase(it);
  channels_by_id_.erase(channel->ChannelId());

  auto* rudp = dynamic_cast<ReliableUdpChannel*>(channel.get());
  if (rudp) {
    hot_channels_.erase(rudp);
  }
  dirty_channels_.erase(channel.get());

  const bool kSharedFd = (rudp_socket_ && channel->Fd() == rudp_socket_->Fd()) ||
                         (udp_socket_ && channel->Fd() == udp_socket_->Fd());
  if (!kSharedFd) (void)dispatcher_.Deregister(channel->Fd());
  channel->Condemn();

  if (rudp && rudp->HasUnackedReliablePackets()) {
    condemned_rudp_by_addr_[addr] = rudp;
  }
  condemned_.push_back({std::move(channel), Clock::now()});

  while (condemned_.size() > kMaxCondemnedChannels) {
    ATLAS_LOG_WARNING(
        "NetworkInterface: condemned channel list at capacity ({}), "
        "force-closing oldest entry",
        kMaxCondemnedChannels);
    EraseCondemnedRudpIndex(condemned_.front().channel.get());
    condemned_.pop_front();
  }
}

void NetworkInterface::EraseCondemnedRudpIndex(Channel* channel) {
  auto* rudp = dynamic_cast<ReliableUdpChannel*>(channel);
  if (!rudp) return;
  auto idx_it = condemned_rudp_by_addr_.find(rudp->RemoteAddress());
  if (idx_it != condemned_rudp_by_addr_.end() && idx_it->second == rudp) {
    condemned_rudp_by_addr_.erase(idx_it);
  }
}

auto NetworkInterface::ShouldDeleteCondemned(const Channel& channel, TimePoint condemned_at,
                                             TimePoint now) const -> bool {
  return !channel.HasUnackedReliablePackets() || channel.HasRemoteFailed() ||
         (now - condemned_at) >= kCondemnAgeLimit;
}

void NetworkInterface::ProcessCondemnedChannels() {
  const auto now = Clock::now();
  for (auto it = condemned_.begin(); it != condemned_.end();) {
    if (!ShouldDeleteCondemned(*it->channel, it->condemned_at, now)) {
      ++it;
      continue;
    }
    if ((now - it->condemned_at) >= kCondemnAgeLimit && it->channel->HasUnackedReliablePackets()) {
      ATLAS_LOG_WARNING("Condemned channel {} hit age limit with unacked packets — discarding",
                        it->channel->RemoteAddress().ToString());
    }
    EraseCondemnedRudpIndex(it->channel.get());
    it = condemned_.erase(it);
  }
}

auto NetworkInterface::DatagramRecvBuffer() -> std::span<std::byte> {
  if (!datagram_recv_scratch_) {
    datagram_recv_scratch_ = StreamBufferPool::Instance().Acquire(kMaxDatagramSize);
  }

  return {datagram_recv_scratch_.data(), datagram_recv_scratch_.Capacity()};
}

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
  auto now = Clock::now();
  std::erase_if(rate_trackers_, [now](const auto& kv) {
    return (now - kv.second.window_start) > std::chrono::seconds(2);
  });
}

}  // namespace atlas
