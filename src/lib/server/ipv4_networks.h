#ifndef ATLAS_LIB_SERVER_IPV4_NETWORKS_H_
#define ATLAS_LIB_SERVER_IPV4_NETWORKS_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "foundation/error.h"

namespace atlas {

struct IPv4Network {
  uint32_t network_host_order{0};
  uint32_t mask_host_order{0xFFFFFFFFu};
  uint8_t prefix_length{32};
  std::string spec;

  [[nodiscard]] auto contains(uint32_t ip_network_order) const -> bool;
};

class IPv4NetworkSet {
 public:
  auto Add(std::string_view spec) -> Result<void>;
  auto AddAll(const std::vector<std::string>& specs) -> Result<void>;

  [[nodiscard]] auto contains(uint32_t ip_network_order) const -> bool;
  [[nodiscard]] auto empty() const -> bool { return networks_.empty(); }
  [[nodiscard]] auto size() const -> std::size_t { return networks_.size(); }

 private:
  std::vector<IPv4Network> networks_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_SERVER_IPV4_NETWORKS_H_
