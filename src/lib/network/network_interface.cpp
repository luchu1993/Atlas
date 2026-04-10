#include "network/network_interface.hpp"

#include "foundation/log.hpp"
#include "network/event_dispatcher.hpp"
#include "network/tcp_channel.hpp"
#include "network/udp_channel.hpp"

#include <algorithm>
#include <array>

namespace atlas
{

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

    auto conn = sock->connect(addr);
    if (!conn && conn.error().code() != ErrorCode::WouldBlock)
    {
        return conn.error();
    }

    auto channel =
        std::make_unique<TcpChannel>(dispatcher_, interface_table_, std::move(*sock), addr);

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

// ============================================================================
// Rate limiting
// ============================================================================

void NetworkInterface::set_rate_limit(uint32_t max_per_second)
{
    rate_limit_ = max_per_second;
}

// ============================================================================
// Shutdown
// ============================================================================

void NetworkInterface::prepare_for_shutdown()
{
    shutting_down_ = true;

    // Condemn all active channels
    std::vector<Address> addrs;
    addrs.reserve(channels_.size());
    for (auto& [addr, _] : channels_)
    {
        addrs.push_back(addr);
    }
    for (auto& addr : addrs)
    {
        condemn_channel(addr);
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
    while (true)
    {
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

        auto channel = std::make_unique<TcpChannel>(dispatcher_, interface_table_,
                                                    std::move(peer_sock), peer_addr);

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

        ATLAS_LOG_INFO("Accepted TCP connection from {}", peer_addr.to_string());
        channels_[peer_addr] = std::move(channel);
    }
}

void NetworkInterface::on_udp_readable()
{
    std::array<std::byte, 2048> buf{};
    while (true)
    {
        auto result = udp_socket_->recv_from(buf);
        if (!result)
        {
            if (result.error().code() == ErrorCode::WouldBlock)
            {
                break;
            }
            ATLAS_LOG_WARNING("UDP recv error: {}", result.error().message());
            break;
        }

        auto [bytes, src_addr] = *result;
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
            channel->set_disconnect_callback([this](Channel& ch) { on_channel_disconnect(ch); });
            channel->activate();
            auto [inserted_it, _] = channels_.emplace(src_addr, std::move(channel));
            it = inserted_it;
        }

        auto* udp_ch = static_cast<UdpChannel*>(it->second.get());
        udp_ch->on_datagram_received(std::span<const std::byte>(buf.data(), bytes));
    }
}

// ============================================================================
// Channel lifecycle
// ============================================================================

void NetworkInterface::on_channel_disconnect(Channel& channel)
{
    condemn_channel(channel.remote_address());
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

    // Deregister fd from dispatcher before moving to condemned
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
        condemned_.erase(condemned_.begin());
    }
}

void NetworkInterface::process_condemned_channels()
{
    auto now = Clock::now();
    std::erase_if(condemned_, [now](const CondemnedEntry& entry)
                  { return (now - entry.condemned_at) >= kCondemnTimeout; });
}

// ============================================================================
// Rate limiting
// ============================================================================

auto NetworkInterface::check_rate_limit(uint32_t ip) -> bool
{
    auto now = Clock::now();
    auto& tracker = rate_trackers_[ip];

    // Reset window if elapsed
    if (now - tracker.window_start >= std::chrono::seconds(1))
    {
        tracker.count = 0;
        tracker.window_start = now;
    }

    ++tracker.count;
    return tracker.count <= rate_limit_;
}

void NetworkInterface::cleanup_stale_rate_trackers()
{
    // Remove entries whose window started more than 2 seconds ago (i.e. inactive IPs).
    auto now = Clock::now();
    std::erase_if(rate_trackers_, [now](const auto& kv)
                  { return (now - kv.second.window_start) > std::chrono::seconds(2); });
}

}  // namespace atlas
