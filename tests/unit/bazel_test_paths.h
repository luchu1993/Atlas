#ifndef ATLAS_TESTS_BAZEL_TEST_PATHS_H_
#define ATLAS_TESTS_BAZEL_TEST_PATHS_H_

// Resolves Bazel runfile paths to absolute filesystem paths.
// On Windows, Bazel uses a MANIFEST file (no symlinks), so LoadLibrary()
// cannot resolve runfiles-relative paths.  This helper parses the manifest
// to find the absolute path.
//
// Usage:
//   auto path = BazelRlocation("_main/src/lib/clrscript/atlas_engine.dll");
//   // Returns absolute path or empty string if not found.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace atlas::test {

/// Look up a runfile key in the Bazel MANIFEST and return the absolute path.
/// Falls back to the key itself if no manifest is found (e.g. CMake builds).
inline std::filesystem::path BazelRlocation(std::string_view key) {
  const char* manifest = std::getenv("RUNFILES_MANIFEST_FILE");
  if (!manifest) {
    // Not running under Bazel, or runfiles dir mode — return key as-is.
    return std::filesystem::path(key);
  }

  std::ifstream file(manifest);
  std::string line;
  while (std::getline(file, line)) {
    // Each line: <key> <space> <absolute-path>
    auto sep = line.find(' ');
    if (sep == std::string::npos) continue;
    if (std::string_view(line.data(), sep) == key) {
      return std::filesystem::path(line.substr(sep + 1));
    }
  }

  // Key not found — return as-is (may work if the file exists at that path).
  return std::filesystem::path(key);
}

/// Convenience: resolve a runfile and return its parent directory.
inline std::filesystem::path BazelRlocationDir(std::string_view key) {
  return BazelRlocation(key).parent_path();
}

}  // namespace atlas::test

#endif  // ATLAS_TESTS_BAZEL_TEST_PATHS_H_
