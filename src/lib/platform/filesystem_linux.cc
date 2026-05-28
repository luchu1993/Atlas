#include "platform/filesystem.h"

#include <cerrno>
#include <cstring>
#include <format>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace atlas::fs {

struct ScopedFileLock::Impl {
  ~Impl() {
    if (fd >= 0) {
      (void)::flock(fd, LOCK_UN);
      (void)::close(fd);
    }
  }

  int fd{-1};
  stdfs::path path;
};

ScopedFileLock::ScopedFileLock() = default;

ScopedFileLock::ScopedFileLock(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

ScopedFileLock::~ScopedFileLock() = default;

ScopedFileLock::ScopedFileLock(ScopedFileLock&& other) noexcept
    : impl_(std::move(other.impl_)) {}

auto ScopedFileLock::operator=(ScopedFileLock&& other) noexcept -> ScopedFileLock& {
  if (this == &other) return *this;
  impl_ = std::move(other.impl_);
  return *this;
}

auto ScopedFileLock::TryAcquire(const stdfs::path& path, std::string_view content)
    -> Result<ScopedFileLock> {
  if (path.empty()) {
    return Error{ErrorCode::kInvalidArgument, "ScopedFileLock requires a path"};
  }
  std::error_code ec;
  const auto parent = path.parent_path();
  if (!parent.empty()) stdfs::create_directories(parent, ec);
  if (ec) {
    return Error{ErrorCode::kIoError,
                 std::format("Failed to create lock directory {}: {}",
                             parent.string(), ec.message())};
  }
  const auto path_string = path.string();
  int fd = ::open(path_string.c_str(), O_RDWR | O_CREAT, 0600);
  if (fd < 0) {
    return Error{ErrorCode::kIoError,
                 std::format("Failed to open file lock {}: {}", path.string(),
                             std::strerror(errno))};
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    const int err = errno;
    (void)::close(fd);
    if (err == EWOULDBLOCK || err == EAGAIN) {
      return Error{ErrorCode::kAlreadyExists,
                   std::format("File lock is already held: {}", path.string())};
    }
    return Error{ErrorCode::kIoError,
                 std::format("Failed to acquire file lock {}: {}", path.string(),
                             std::strerror(err))};
  }
  if (::ftruncate(fd, 0) != 0) {
    const int err = errno;
    (void)::flock(fd, LOCK_UN);
    (void)::close(fd);
    return Error{ErrorCode::kIoError,
                 std::format("Failed to truncate file lock {}: {}", path.string(),
                             std::strerror(err))};
  }
  if (!content.empty()) {
    const auto* data = content.data();
    auto remaining = content.size();
    while (remaining > 0) {
      const auto written = ::write(fd, data, remaining);
      if (written <= 0) {
        const int err = errno;
        (void)::flock(fd, LOCK_UN);
        (void)::close(fd);
        return Error{ErrorCode::kIoError,
                     std::format("Failed to write file lock {}: {}", path.string(),
                                 std::strerror(err))};
      }
      data += static_cast<std::size_t>(written);
      remaining -= static_cast<std::size_t>(written);
    }
  }
  auto impl = std::make_unique<Impl>();
  impl->fd = fd;
  impl->path = path;
  return ScopedFileLock(std::move(impl));
}

auto ScopedFileLock::IsHeld() const -> bool {
  return impl_ && impl_->fd >= 0;
}

[[nodiscard]] auto AtomicReplaceFile(const stdfs::path& source, const stdfs::path& target)
    -> Result<void> {
  if (source.empty() || target.empty()) {
    return Error{ErrorCode::kInvalidArgument, "AtomicReplaceFile requires source and target"};
  }
  std::error_code ec;
  stdfs::rename(source, target, ec);
  if (ec) {
    return Error{ErrorCode::kIoError,
                 std::format("Failed to replace file {} with {}: {}", target.string(),
                             source.string(), ec.message())};
  }
  return {};
}

}  // namespace atlas::fs
