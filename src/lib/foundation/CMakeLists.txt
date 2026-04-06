# atlas_foundation — Core utilities (logging, memory, containers, time, strings)

atlas_add_library(atlas_foundation
    SOURCES
        error.cpp
        string_utils.cpp
        log.cpp
        log_sinks.cpp
        time.cpp
        timer_queue.cpp
        pool_allocator.cpp
        memory_tracker.cpp
    DEPS
        atlas_platform_config
        Threads::Threads
)
