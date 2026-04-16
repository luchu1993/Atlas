# atlas_coro — C++20 coroutine primitives (Task, FireAndForget, ScopeGuard, Cancellation)

atlas_add_library(atlas_coro
    SOURCES
        pending_rpc_registry.cc
    DEPS
        atlas_foundation
        atlas_network
)
