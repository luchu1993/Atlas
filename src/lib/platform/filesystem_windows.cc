#include "platform/filesystem.h"

#include <format>
#include <utility>

#include <windows.h>

namespace atlas::fs {

struct ScopedFileLock::Impl {
  ~Impl() {
    if (handle != INVALID_HANDLE_VALUE) {
      ::CloseHandle(handle);
    }
  }

  HANDLE handle{INVALID_HANDLE_VALUE};
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
  HANDLE handle = ::CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD err = ::GetLastError();
    if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION) {
      return Error{ErrorCode::kAlreadyExists,
                   std::format("File lock is already held: {}", path.string())};
    }
    return Error{ErrorCode::kIoError,
                 std::format("Failed to acquire file lock {}: {}", path.string(), err)};
  }
  if (!content.empty()) {
    DWORD written = 0;
    if (!::WriteFile(handle, content.data(), static_cast<DWORD>(content.size()), &written,
                     nullptr) ||
        written != content.size()) {
      ::CloseHandle(handle);
      return Error{ErrorCode::kIoError,
                   std::format("Failed to write file lock {}", path.string())};
    }
  }
  auto impl = std::make_unique<Impl>();
  impl->handle = handle;
  impl->path = path;
  return ScopedFileLock(std::move(impl));
}

auto ScopedFileLock::IsHeld() const -> bool {
  return impl_ && impl_->handle != INVALID_HANDLE_VALUE;
}

[[nodiscard]] auto AtomicReplaceFile(const stdfs::path& source, const stdfs::path& target)
    -> Result<void> {
  if (source.empty() || target.empty()) {
    return Error{ErrorCode::kInvalidArgument, "AtomicReplaceFile requires source and target"};
  }
  if (!MoveFileExW(source.wstring().c_str(), target.wstring().c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return Error{ErrorCode::kIoError,
                 std::format("Failed to replace file {} with {}: {}", target.string(),
                             source.string(), GetLastError())};
  }
  return {};
}

}  // namespace atlas::fs
