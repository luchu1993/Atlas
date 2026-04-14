#pragma once

#include <functional>
#include <utility>

namespace atlas
{

// ============================================================================
// ScopeGuard — RAII cleanup / rollback guard
// ============================================================================

class ScopeGuard
{
public:
    explicit ScopeGuard(std::function<void()> cleanup) : cleanup_(std::move(cleanup)) {}

    ~ScopeGuard()
    {
        if (!dismissed_ && cleanup_)
            cleanup_();
    }

    void dismiss() noexcept { dismissed_ = true; }

    ScopeGuard(ScopeGuard&& other) noexcept
        : cleanup_(std::move(other.cleanup_)), dismissed_(other.dismissed_)
    {
        other.dismissed_ = true;
    }

    ScopeGuard& operator=(ScopeGuard&&) = delete;
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;

private:
    std::function<void()> cleanup_;
    bool dismissed_{false};
};

// Convenience macro: ATLAS_SCOPE_EXIT { cleanup_code; };
#define ATLAS_SCOPE_EXIT_CAT2(x, y) x##y
#define ATLAS_SCOPE_EXIT_CAT(x, y) ATLAS_SCOPE_EXIT_CAT2(x, y)
#define ATLAS_SCOPE_EXIT auto ATLAS_SCOPE_EXIT_CAT(scope_exit_, __LINE__) = ::atlas::ScopeGuard

}  // namespace atlas
