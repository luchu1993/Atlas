#pragma once

#include "foundation/error.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace atlas
{

struct IPv4Network
{
    uint32_t network_host_order{0};
    uint32_t mask_host_order{0xFFFFFFFFu};
    uint8_t prefix_length{32};
    std::string spec;

    [[nodiscard]] auto contains(uint32_t ip_network_order) const -> bool;
};

class IPv4NetworkSet
{
public:
    auto add(std::string_view spec) -> Result<void>;
    auto add_all(const std::vector<std::string>& specs) -> Result<void>;

    [[nodiscard]] auto contains(uint32_t ip_network_order) const -> bool;
    [[nodiscard]] auto empty() const -> bool { return networks_.empty(); }
    [[nodiscard]] auto size() const -> std::size_t { return networks_.size(); }

private:
    std::vector<IPv4Network> networks_;
};

}  // namespace atlas
