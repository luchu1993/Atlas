# atlas_foundation — Core utilities (logging, memory, containers, time, strings)

atlas_add_library(atlas_foundation
    SOURCES
        error.cc
        string_utils.cc
        log.cc
        log_sinks.cc
        runtime.cc
        clock.cc
        timer_queue.cc
        pool_allocator.cc
        memory_tracker.cc
    DEPS
        atlas_platform_config
        Threads::Threads
)
