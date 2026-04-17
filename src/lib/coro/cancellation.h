#ifndef ATLAS_LIB_CORO_CANCELLATION_H_
#define ATLAS_LIB_CORO_CANCELLATION_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace atlas {

class CancellationToken;

// ============================================================================
// CancellationState — shared state between source and tokens
// ============================================================================

struct CancellationState {
  struct Entry {
    uint64_t id{0};
    std::function<void()> callback;
  };

  bool cancelled{false};
  uint64_t next_id{1};
  std::vector<Entry> callbacks;
};

// ============================================================================
// CancelRegistration — RAII handle that deregisters on destruction
// ============================================================================

class CancelRegistration {
 public:
  CancelRegistration() = default;

  ~CancelRegistration() {
    if (state_ && id_ != 0) {
      std::erase_if(state_->callbacks,
                    [id = id_](const CancellationState::Entry& e) { return e.id == id; });
    }
  }

  CancelRegistration(CancelRegistration&& other) noexcept
      : state_(std::move(other.state_)), id_(std::exchange(other.id_, 0)) {}

  CancelRegistration& operator=(CancelRegistration&& other) noexcept {
    if (this != &other) {
      // Deregister current
      if (state_ && id_ != 0) {
        std::erase_if(state_->callbacks,
                      [id = id_](const CancellationState::Entry& e) { return e.id == id; });
      }
      state_ = std::move(other.state_);
      id_ = std::exchange(other.id_, 0);
    }
    return *this;
  }

  CancelRegistration(const CancelRegistration&) = delete;
  CancelRegistration& operator=(const CancelRegistration&) = delete;

 private:
  friend class CancellationToken;

  CancelRegistration(std::shared_ptr<CancellationState> state, uint64_t id)
      : state_(std::move(state)), id_(id) {}

  std::shared_ptr<CancellationState> state_;
  uint64_t id_{0};
};

// ============================================================================
// CancellationSource — owner calls request_cancellation()
// ============================================================================

class CancellationSource {
 public:
  CancellationSource() : state_(std::make_shared<CancellationState>()) {}

  void RequestCancellation() {
    if (state_->cancelled) return;
    state_->cancelled = true;

    // Copy callbacks before invoking — a callback might deregister others
    auto callbacks = std::move(state_->callbacks);
    state_->callbacks.clear();
    for (auto& entry : callbacks) {
      if (entry.callback) entry.callback();
    }
  }

  [[nodiscard]] auto Token() const -> CancellationToken;

  [[nodiscard]] auto IsCancellationRequested() const -> bool { return state_->cancelled; }

 private:
  std::shared_ptr<CancellationState> state_;
};

// ============================================================================
// CancellationToken — read-only view, passed to awaitables
// ============================================================================

class CancellationToken {
 public:
  CancellationToken() = default;

  [[nodiscard]] auto IsCancelled() const -> bool { return state_ && state_->cancelled; }

  [[nodiscard]] auto OnCancel(std::function<void()> callback) -> CancelRegistration {
    if (!state_) return {};

    if (state_->cancelled) {
      callback();
      return {};
    }

    auto id = state_->next_id++;
    state_->callbacks.push_back({id, std::move(callback)});
    return CancelRegistration{state_, id};
  }

  [[nodiscard]] auto IsValid() const -> bool { return state_ != nullptr; }

 private:
  friend class CancellationSource;

  explicit CancellationToken(std::shared_ptr<CancellationState> state) : state_(std::move(state)) {}

  std::shared_ptr<CancellationState> state_;
};

// Inline definition (needs CancellationToken to be complete)
inline auto CancellationSource::Token() const -> CancellationToken {
  return CancellationToken{state_};
}

}  // namespace atlas

#endif  // ATLAS_LIB_CORO_CANCELLATION_H_
