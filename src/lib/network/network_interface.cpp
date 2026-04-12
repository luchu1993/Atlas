#include "network/network_interface.hpp"

#include "foundation/log.hpp"
#include "network/event_dispatcher.hpp"
#include "network/reliable_udp.hpp"
#include "network/tcp_channel.hpp"
#include "network/udp_channel.hpp"

#include <algorithm>

namespace atlas
{

namespace
{

void apply_rudp_profile(ReliableUdpChannel& channel, const NetworkInterface::RudpProfile& profile)
{
    channel.set_nocwnd(profile.nocwnd);
    channel.set_send_window(profile.send_window);
    channel.set_recv_window(profile.recv_window);
}

}  // namespace

// ============================================================================
// Constructor / Destructor
// ============================================================================

NetworkInterface::NetworkInterface(EventDispatcher& dispatcher)
    : dispatcher_(dispatcher), registration_(dispatcher_.add_frequent_task(this))
{
}

NetworkInterface::~NetworkInterface()
{
    // registration_ destructor automatically calls dispatcher_.remove_frequent_task(this).
    // Explicitly reset here to ensure removal happens before channel teardown.
    registration_.reset();

    channels_.clear();
    channels_by_id_.clear();
    condemned_.clear();

    if (tcp_listen_socket_)
    {
        (void)dispatcher_.deregister(tcp_listen_socket_->fd());
        tcp_listen_socket_->close();
        tcp_listen_socket_.reset();
    }

    if (udp_socket_)
    {
        (void)dispatcher_.deregister(udp_socket_->fd());
        udp_socket_->close();
        udp_socket_.reset();
    }

    if (rudp_socket_)
    {
        (void)dispatcher_.deregister(rudp_socket_->fd());
        rudp_socket_->close();
        rudp_socket_.reset();
    }
}

// ============================================================================
// TCP server
// ============================================================================

auto NetworkInterface::start_tcp_server(const Address& addr) -> Result<void>
{
    auto sock = Socket::create_tcp();
    if (!sock)
    {
        return sock.error();
    }

    if (auto r = sock->set_reuse_addr(true); !r)
        return r.error();
    if (auto r = sock->set_non_blocking(true); !r)
        return r.error();

    auto bind_result = sock->bind(addr);
    if (!bind_result)
    {
        return bind_result.error();
    }

    auto listen_result = sock->listen();
    if (!listen_result)
    {
        return listen_result.error();
    }

    auto local = sock->local_address();
    if (!local)
    {
        return local.error();
    }
    tcp_address_ = *local;

    tcp_listen_socket_ = std::move(*sock);

    // Register for accept events
    auto reg = dispatcher_.register_reader(tcp_listen_socket_->fd(),
                                           [this](FdHandle, IOEvent) { on_tcp_accept(); });
    if (!reg)
    {
        return reg.error();
    }

    ATLAS_LOG_INFO("TCP server listening on {}", tcp_address_.to_string());
    return Result<void>{};
}

// ============================================================================
// TCP client (async connect)
// ============================================================================

auto NetworkInterface::connect_tcp(const Address& addr) -> Result<TcpChannel*>
{
    if (shutting_down_)
    {
        return Error(ErrorCode::ChannelCondemned, "Shutting down");
    }

    auto sock = Socket::create_tcp();
    if (!sock)
    {
        return sock.error();
    }

    if (auto r = sock->set_non_blocking(true); !r)
        return r.error();
    if (auto r = sock->set_no_delay(true); !r)
        return r.error();

    auto conn = sock->connect(addr);
    if (!conn && conn.error().code() != ErrorCode::WouldBlock)
    {
        return conn.error();
    }

    auto channel =
        std::make_unique<TcpChannel>(dispatcher_, interface_table_, std::move(*sock), addr);
    channel->set_channel_id(next_channel_id_++);

    auto reg = dispatcher_.register_reader(channel->fd(),
                                           [ch = channel.get()](FdHandle, IOEvent events)
                                           {
                                               if ((events & IOEvent::Readable) != IOEvent::None)
                                               {
                                                   ch->on_readable();
                                               }
                                               if ((events & IOEvent::Writable) != IOEvent::None)
                                               {
                                                   ch->on_writable();
                                               }
                                           });
    if (!reg)
    {
        return reg.error();
    }

    channel->set_disconnect_callback([this](Channel& ch) { on_channel_disconnect(ch); });
    channel->activate();

    auto* raw = channel.get();
    channels_by_id_[raw->channel_id()] = raw;
    channels_[addr] = std::move(channel);
    return raw;
}

// ============================================================================
// UDP endpoint
// ============================================================================

auto NetworkInterface::start_udp(const Address& addr) -> Result<void>
{
    auto sock = Socket::create_udp();
    if (!sock)
    {
        return sock.error();
    }

    if (auto r = sock->set_reuse_addr(true); !r)
        return r.error();
    if (auto r = sock->set_non_blocking(true); !r)
        return r.error();

    auto bind_result = sock->bind(addr);
    if (!bind_result)
    {
        return bind_result.error();
    }

    auto local = sock->local_address();
    if (!local)
    {
        return local.error();
    }
    udp_address_ = *local;

    udp_socket_ = std::move(*sock);

    auto reg = dispatcher_.register_reader(udp_socket_->fd(),
                                           [this](FdHandle, IOEvent) { on_udp_readable(); });
    if (!reg)
    {
        return reg.error();
    }

    ATLAS_LOG_INFO("UDP listening on {}", udp_address_.to_string());
    return Result<void>{};
}

auto NetworkInterface::connect_udp(const Address& addr) -> Result<UdpChannel*>
{
    if (shutting_down_)
        return Error(ErrorCode::ChannelCondemned, "Shutting down");

    // Open shared UDP socket on first use (bind to any port)
    if (!udp_socket_)
    {
        if (auto r = start_udp(Address(0, 0)); !r)
            return r.error();
    }

    // Re-use existing channel if already targeting this peer
    if (auto it = channels_.find(addr); it != channels_.end())
        return static_cast<UdpChannel*>(it->second.get());

    auto channel = std::make_unique<UdpChannel>(dispatcher_, interface_table_, *udp_socket_, addr);
    channel->set_channel_id(next_channel_id_++);
    channel->set_disconnect_callback([this](Channel& ch) { on_channel_disconnect(ch); });
    channel->activate();

    auto* raw = static_cast<UdpChannel*>(channel.get());
    channels_by_id_[raw->channel_id()] = raw;
    channels_[addr] = std::move(channel);
    return raw;
}

// ============================================================================
// Channel access
// ============================================================================

auto NetworkInterface::find_channel(const Address& addr) -> Channel*
{
    auto it = channels_.find(addr);
    if (it != channels_.end())
    {
        return it->second.get();
    }
    return nullptr;
}

auto NetworkInterface::find_channel(ChannelId id) -> Channel*
{
    auto it = channels_by_id_.find(id);
    if (it != channels_by_id_.end())
    {
        return it->second;
    }
    return nullptr;
}

auto NetworkInterface::channel_count() const -> size_t
{
    return channels_.size();
}

// ============================================================================
// Addresses
// ============================================================================

auto NetworkInterface::tcp_address() const -> Address
{
    return tcp_address_;
}

auto NetworkInterface::udp_address() const -> Address
{
    return udp_address_;
}

auto NetworkInterface::rudp_address() const -> Address
{
    return rudp_address_;
}

// ============================================================================
// RUDP server / client
// ============================================================================

auto NetworkInterface::start_rudp_server(const Address& addr, const RudpProfile& accept_profile)
    -> Result<void>
{
    if (rudp_socket_)
    {
        return Error(ErrorCode::AlreadyExists, "RUDP socket already open");
    }

    auto sock = Socket::create_udp();
    if (!sock)
        return sock.error();

    if (auto r = sock->set_reuse_addr(true); !r)
        return r.error();
    if (auto r = sock->set_non_blocking(true); !r)
        return r.error();
    if (auto r = sock->set_recv_buffer_size(4 * 1024 * 1024); !r)
        ATLAS_LOG_WARNING("RUDP: failed to set recv buffer size: {}", r.error().message());
    if (auto r = sock->set_send_buffer_size(4 * 1024 * 1024); !r)
        ATLAS_LOG_WARNING("RUDP: failed to set send buffer size: {}", r.error().message());

    if (auto r = sock->bind(addr); !r)
        return r.error();

    auto local = sock->local_address();
    if (!local)
        return local.error();
    rudp_address_ = *local;

    rudp_socket_ = std::move(*sock);
    rudp_server_mode_ = true;
    rudp_accept_profile_ = accept_profile;

    auto reg = dispatcher_.register_reader(rudp_socket_->fd(),
                                           [this](FdHandle, IOEvent) { on_rudp_readable(); });
    if (!reg)
        return reg.error();

    ATLAS_LOG_INFO("RUDP server listening on {}", rudp_address_.to_string());
    return Result<void>{};
}

auto NetworkInterface::connect_rudp(const Address& addr, const RudpProfile& profile)
    -> Result<ReliableUdpChannel*>
{
    if (shutting_down_)
        return Error(ErrorCode::ChannelCondemned, "Shutting down");

    // Open shared RUDP socket on first use (client side: bind to any port)
    if (!rudp_socket_)
    {
        auto sock = Socket::create_udp();
        if (!sock)
            return sock.error();

        if (auto r = sock->set_non_blocking(true); !r)
            return r.error();
        if (auto r = sock->set_recv_buffer_size(4 * 1024 * 1024); !r)
            ATLAS_LOG_WARNING("RUDP: failed to set recv buffer size: {}", r.error().message());
        if (auto r = sock->set_send_buffer_size(4 * 1024 * 1024); !r)
            ATLAS_LOG_WARNING("RUDP: failed to set send buffer size: {}", r.error().message());

        const Address bind_addr = rudp_client_bind_address_.value_or(Address(0, 0));
        if (auto r = sock->bind(bind_addr); !r)
            return r.error();

        auto local = sock->local_address();
        if (!local)
            return local.error();
        rudp_address_ = *local;

        rudp_socket_ = std::move(*sock);

        auto reg = dispatcher_.register_reader(rudp_socket_->fd(),
                                               [this](FdHandle, IOEvent) { on_rudp_readable(); });
        if (!reg)
            return reg.error();
    }

    // Re-use existing channel if already connected to this peer
    if (auto it = channels_.find(addr); it != channels_.end())
    {
        return static_cast<ReliableUdpChannel*>(it->second.get());
    }

    auto channel =
        std::make_unique<ReliableUdpChannel>(dispatcher_, interface_table_, *rudp_socket_, addr);
    channel->set_channel_id(next_channel_id_++);
    channel->set_disconnect_callback([this](Channel& ch) { on_channel_disconnect(ch); });
    apply_rudp_profile(*channel, profile);
    channel->activate();

    auto* raw = static_cast<ReliableUdpChannel*>(channel.get());
    channels_by_id_[raw->channel_id()] = raw;
    channels_[addr] = std::move(channel);
    return raw;
}

auto NetworkInterface::internet_rudp_profile() -> RudpProfile
{
    return RudpProfile{};
}

auto NetworkInterface::cluster_rudp_profile() -> RudpProfile
{
    RudpProfile profile;
    profile.nocwnd = true;
    profile.send_window = 4096;
    profile.recv_window = 4096;
    return profile;
}

void NetworkInterface::set_rudp_client_bind_address(const Address& addr)
{
    rudp_client_bind_address_ = addr;
}

auto NetworkInterface::connect_rudp_nocwnd(const Address& addr) -> Result<ReliableUdpChannel*>
{
    // Re-use existing channel without changing its nocwnd flag
    if (auto it = channels_.find(addr); it != channels_.end())
        return static_cast<ReliableUdpChannel*>(it->second.get());

    auto result = connect_rudp(addr, cluster_rudp_profile());
    if (!result)
        return result;
    return result;
}

// ============================================================================
// Rate limiting
// ============================================================================

void NetworkInterface::set_rate_limit(uint32_t max_per_second)
{
    rate_limit_ = max_per_second;
}

void NetworkInterface::set_accept_callback(AcceptCallback cb)
{
    accept_callback_ = std::move(cb);
}

void NetworkInterface::set_disconnect_callback(DisconnectCallback cb)
{
    disconnect_callback_ = std::move(cb);
}

// ============================================================================
// Shutdown
// ============================================================================

void NetworkInterface::prepare_for_shutdown()
{
    shutting_down_ = true;

    while (!channels_.empty())
    {
        condemn_channel(channels_.begin()->first);
    }

    ATLAS_LOG_INFO("NetworkInterface preparing for shutdown, {} channels condemned",
                   condemned_.size());
}

// ============================================================================
// FrequentTask
// ============================================================================

void NetworkInterface::do_task()
{
    process_condemned_channels();

    if (rate_limit_ > 0)
    {
        auto now = Clock::now();
        if (now - last_rate_cleanup_ >= kRateCleanupInterval)
        {
            cleanup_stale_rate_trackers();
            last_rate_cleanup_ = now;
        }
    }
}

// ============================================================================
// IO callbacks
// ============================================================================

void NetworkInterface::on_tcp_accept()
{
    std::size_t accepts = 0;
    while (true)
    {
        if (callback_budget_exhausted(accepts, kMaxAcceptsPerCallback))
        {
            break;
        }

        auto result = tcp_listen_socket_->accept();
        if (!result)
        {
            if (result.error().code() == ErrorCode::WouldBlock)
            {
                break;
            }
            ATLAS_LOG_WARNING("TCP accept error: {}", result.error().message());
            break;
        }

        auto& [peer_sock, peer_addr] = *result;
        ++accepts;

        // Rate check
        if (rate_limit_ > 0 && !check_rate_limit(peer_addr.ip()))
        {
            ATLAS_LOG_WARNING("Rate limited connection from {}", peer_addr.to_string());
            continue;  // Socket destructor closes it
        }

        if (shutting_down_)
        {
            continue;
        }

        (void)peer_sock.set_no_delay(true);

        auto channel = std::make_unique<TcpChannel>(dispatcher_, interface_table_,
                                                    std::move(peer_sock), peer_addr);
        channel->set_channel_id(next_channel_id_++);

        auto reg =
            dispatcher_.register_reader(channel->fd(),
                                        [ch = channel.get()](FdHandle, IOEvent events)
                                        {
                                            if ((events & IOEvent::Readable) != IOEvent::None)
                                            {
                                                ch->on_readable();
                                            }
                                            if ((events & IOEvent::Writable) != IOEvent::None)
                                            {
                                                ch->on_writable();
                                            }
                                        });
        if (!reg)
        {
            ATLAS_LOG_ERROR("Failed to register channel fd: {}", reg.error().message());
            continue;
        }

        channel->set_disconnect_callback([this](Channel& ch) { on_channel_disconnect(ch); });
        channel->activate();

        ATLAS_LOG_DEBUG("Accepted TCP connection from {}", peer_addr.to_string());
        channels_by_id_[channel->channel_id()] = channel.get();
        channels_[peer_addr] = std::move(channel);
        if (accept_callback_)
        {
            accept_callback_(*channels_[peer_addr]);
        }
    }
}

void NetworkInterface::on_udp_readable()
{
    auto recv_buffer = datagram_recv_buffer();
    std::size_t datagrams = 0;
    while (true)
    {
        if (callback_budget_exhausted(datagrams, kMaxDatagramsPerCallback))
        {
            break;
        }

        auto result = udp_socket_->recv_from(recv_buffer);
        if (!result)
        {
            if (result.error().code() == ErrorCode::WouldBlock)
            {
                break;
            }
            if (result.error().code() == ErrorCode::ConnectionReset)
            {
                ++datagrams;
                continue;
            }
            ATLAS_LOG_WARNING("UDP recv error: {}", result.error().message());
            break;
        }

        auto [bytes, src_addr] = *result;
        ++datagrams;
        if (bytes == 0)
        {
            continue;
        }

        // Rate check
        if (rate_limit_ > 0 && !check_rate_limit(src_addr.ip()))
        {
            continue;
        }

        // Find or create UDP channel for this peer
        auto it = channels_.find(src_addr);
        if (it == channels_.end())
        {
            if (shutting_down_)
            {
                continue;
            }

            auto channel =
                std::make_unique<UdpChannel>(dispatcher_, interface_table_, *udp_socket_, src_addr);
            channel->set_channel_id(next_channel_id_++);
            channel->set_disconnect_callback([this](Channel& ch) { on_channel_disconnect(ch); });
            channel->activate();
            channels_by_id_[channel->channel_id()] = channel.get();
            auto [inserted_it, _] = channels_.emplace(src_addr, std::move(channel));
            it = inserted_it;
        }

        auto* udp_ch = static_cast<UdpChannel*>(it->second.get());
        udp_ch->on_datagram_received(recv_buffer.first(bytes));
    }
}

void NetworkInterface::on_rudp_readable()
{
    auto recv_buffer = datagram_recv_buffer();
    std::size_t datagrams = 0;
    while (true)
    {
        if (callback_budget_exhausted(datagrams, kMaxDatagramsPerCallback))
        {
            break;
        }

        auto result = rudp_socket_->recv_from(recv_buffer);
        if (!result)
        {
            if (result.error().code() == ErrorCode::WouldBlock)
                break;
            if (result.error().code() == ErrorCode::ConnectionReset)
            {
                ++datagrams;
                continue;
            }
            ATLAS_LOG_WARNING("RUDP recv error: {}", result.error().message());
            break;
        }

        auto [bytes, src_addr] = *result;
        ++datagrams;
        if (bytes == 0)
            continue;

        if (rate_limit_ > 0 && !check_rate_limit(src_addr.ip()))
            continue;

        auto it = channels_.find(src_addr);
        if (it == channels_.end())
        {
            if (!rudp_server_mode_ || shutting_down_)
                continue;

            auto channel = std::make_unique<ReliableUdpChannel>(dispatcher_, interface_table_,
                                                                *rudp_socket_, src_addr);
            channel->set_channel_id(next_channel_id_++);
            channel->set_disconnect_callback([this](Channel& ch) { on_channel_disconnect(ch); });
            apply_rudp_profile(*channel, rudp_accept_profile_);
            channel->activate();

            ATLAS_LOG_DEBUG("RUDP: new peer {}", src_addr.to_string());
            channels_by_id_[channel->channel_id()] = channel.get();
            auto [inserted_it, _] = channels_.emplace(src_addr, std::move(channel));
            it = inserted_it;
            if (accept_callback_)
                accept_callback_(*it->second);
        }

        auto* rudp_ch = static_cast<ReliableUdpChannel*>(it->second.get());
        rudp_ch->on_datagram_received(recv_buffer.first(bytes));
    }
}

// ============================================================================
// Channel lifecycle
// ============================================================================

void NetworkInterface::on_channel_disconnect(Channel& channel)
{
    condemn_channel(channel.remote_address());
    if (disconnect_callback_)
    {
        disconnect_callback_(channel);
    }
}

void NetworkInterface::condemn_channel(const Address& addr)
{
    auto it = channels_.find(addr);
    if (it == channels_.end())
    {
        return;
    }

    auto channel = std::move(it->second);
    channels_.erase(it);
    channels_by_id_.erase(channel->channel_id());

    // RUDP channels share a single socket — deregistering it would break all
    // other RUDP channels. Only deregister for TCP (owns its own socket).
    const bool shared_fd = rudp_socket_ && channel->fd() == rudp_socket_->fd();
    if (!shared_fd)
        (void)dispatcher_.deregister(channel->fd());
    channel->condemn();

    condemned_.push_back({std::move(channel), Clock::now()});

    // Enforce the cap: force-close the oldest entry if we are over the limit.
    while (condemned_.size() > kMaxCondemnedChannels)
    {
        ATLAS_LOG_WARNING(
            "NetworkInterface: condemned channel list at capacity ({}), "
            "force-closing oldest entry",
            kMaxCondemnedChannels);
        condemned_.pop_front();
    }
}

void NetworkInterface::process_condemned_channels()
{
    auto now = Clock::now();
    while (!condemned_.empty() && (now - condemned_.front().condemned_at) >= kCondemnTimeout)
    {
        condemned_.pop_front();
    }
}

auto NetworkInterface::datagram_recv_buffer() -> std::span<std::byte>
{
    if (!datagram_recv_scratch_)
    {
        datagram_recv_scratch_ = StreamBufferPool::instance().acquire(kMaxDatagramSize);
    }

    return {datagram_recv_scratch_.data(), datagram_recv_scratch_.capacity()};
}

// ============================================================================
// Rate limiting
// ============================================================================

auto NetworkInterface::check_rate_limit(uint32_t ip) -> bool
{
    static constexpr std::size_t kMaxRateTrackers = 100'000;

    if (rate_trackers_.size() >= kMaxRateTrackers && !rate_trackers_.contains(ip))
    {
        return false;
    }

    auto now = Clock::now();
    auto& tracker = rate_trackers_[ip];

    if (now - tracker.window_start >= std::chrono::seconds(1))
    {
        tracker.count = 0;
        tracker.window_start = now;
    }

    ++tracker.count;
    return tracker.count <= rate_limit_;
}

auto NetworkInterface::callback_budget_exhausted(std::size_t processed, std::size_t budget) -> bool
{
    return processed >= budget;
}

void NetworkInterface::cleanup_stale_rate_trackers()
{
    // Remove entries whose window started more than 2 seconds ago (i.e. inactive IPs).
    auto now = Clock::now();
    std::erase_if(rate_trackers_, [now](const auto& kv)
                  { return (now - kv.second.window_start) > std::chrono::seconds(2); });
}

}  // namespace atlas
