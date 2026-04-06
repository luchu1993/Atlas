#include "network/address.hpp"

#include <format>

#if ATLAS_PLATFORM_WINDOWS
#   include <winsock2.h>
#   include <ws2tcpip.h>
#else
#   include <arpa/inet.h>
#   include <netinet/in.h>
#   include <netdb.h>
#   include <sys/socket.h>
#endif

namespace
{

void ensure_network_init()
{
#if ATLAS_PLATFORM_WINDOWS
    struct WinsockInit
    {
        WinsockInit()
        {
            WSADATA wsa;
            WSAStartup(MAKEWORD(2, 2), &wsa);
        }
        ~WinsockInit() { WSACleanup(); }
    };
    [[maybe_unused]] static WinsockInit init;
#endif
}

} // anonymous namespace

namespace atlas
{

const Address Address::NONE{};

Address::Address(std::string_view ip, uint16_t port)
    : port_(port)
{
    std::string ip_str(ip);
    struct in_addr sin_addr{};
    inet_pton(AF_INET, ip_str.c_str(), &sin_addr);
    ip_ = sin_addr.s_addr;
}

Address::Address(const sockaddr_in& sa)
    : ip_(sa.sin_addr.s_addr)
    , port_(ntohs(sa.sin_port))
{
}

auto Address::to_string() const -> std::string
{
    const auto* bytes = reinterpret_cast<const uint8_t*>(&ip_);
    return std::format("{}.{}.{}.{}:{}",
        bytes[0], bytes[1], bytes[2], bytes[3], port_);
}

auto Address::to_sockaddr() const -> sockaddr_in
{
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port_);
    sa.sin_addr.s_addr = ip_;
    return sa;
}

auto Address::resolve(std::string_view hostname, uint16_t port)
    -> Result<Address>
{
    ensure_network_init();
    std::string hostname_str(hostname);

    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = nullptr;
    int rc = getaddrinfo(hostname_str.c_str(), nullptr, &hints, &result);
    if (rc != 0 || result == nullptr)
    {
        return Error(ErrorCode::NotFound,
            std::format("Failed to resolve hostname: {}", hostname_str));
    }

    auto* sa = reinterpret_cast<const sockaddr_in*>(result->ai_addr);
    Address addr(*sa);
    addr.port_ = port;
    freeaddrinfo(result);

    return addr;
}

} // namespace atlas
