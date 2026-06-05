#include "navigation/nav_query.h"

namespace atlas::nav {

auto NullNavQuery::FindPath(const math::Vector3& /*start*/, const math::Vector3& /*end*/,
                            const NavQueryFilter& /*filter*/) const -> NavPath {
  return NavPath{};  // kEmpty: never a fabricated straight line
}

auto NullNavQuery::NearestPoint(const math::Vector3& /*point*/, const math::Vector3& /*half_extents*/,
                                const NavQueryFilter& /*filter*/) const -> NavPoint {
  return NavPoint{};  // on_mesh == false
}

auto NullNavQuery::Raycast(const math::Vector3& /*start*/, const math::Vector3& /*end*/,
                           const NavQueryFilter& /*filter*/) const -> NavRaycastHit {
  return NavRaycastHit{};  // nothing to block against
}

}  // namespace atlas::nav
