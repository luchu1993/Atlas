#ifndef ATLAS_TESTS_TEST_PATHS_H_
#define ATLAS_TESTS_TEST_PATHS_H_

// Resolves test resource paths from environment variables.
// CMake sets these via set_tests_properties(... ENVIRONMENT ...).

#include <cstdlib>
#include <filesystem>
#include <string_view>

namespace atlas::test {

/// Return the path as a filesystem::path.
inline std::filesystem::path ResolvePath(std::string_view path) {
  return std::filesystem::path(path);
}

}  // namespace atlas::test

#endif  // ATLAS_TESTS_TEST_PATHS_H_
