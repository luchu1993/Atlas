# atlas_platform_config — Header-only platform detection (no circular dependency)
add_library(atlas_platform_config INTERFACE)
target_include_directories(atlas_platform_config
    INTERFACE $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/lib>
)
set_target_properties(atlas_platform_config PROPERTIES FOLDER "Libraries")

# atlas_platform — OS abstraction layer (I/O, threading, signals, filesystem)
set(ATLAS_PLATFORM_SOURCES
    filesystem.cpp
    io_poller.cpp
    io_poller_select.cpp
    threading.cpp
    signal_handler.cpp
)

# Platform-specific sources
if(WIN32)
    list(APPEND ATLAS_PLATFORM_SOURCES
        io_poller_wsapoll.cpp
        threading_windows.cpp
        dynamic_library_windows.cpp
    )
elseif(UNIX AND NOT APPLE)
    list(APPEND ATLAS_PLATFORM_SOURCES
        io_poller_epoll.cpp
        io_poller_io_uring.cpp
        threading_linux.cpp
        dynamic_library_linux.cpp
    )
endif()

atlas_add_library(atlas_platform
    SOURCES ${ATLAS_PLATFORM_SOURCES}
    DEPS
        atlas_foundation
        Threads::Threads
)

# On Windows, link ws2_32 for Winsock
if(WIN32)
    target_link_libraries(atlas_platform PUBLIC ws2_32)
endif()

# On Linux, link dl for dlopen/dlsym; optional liburing for io_uring IOPoller
if(UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(URING QUIET IMPORTED_TARGET liburing)
        if(URING_FOUND)
            target_link_libraries(atlas_platform PUBLIC PkgConfig::URING)
            target_compile_definitions(atlas_platform PUBLIC ATLAS_HAS_IO_URING=1)
        endif()
    endif()
    target_link_libraries(atlas_platform PUBLIC ${CMAKE_DL_LIBS})
endif()
