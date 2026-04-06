# atlas_network — Sockets, EventDispatcher, channels, messages, NetworkInterface

atlas_add_library(atlas_network
    SOURCES
        address.cpp
        socket.cpp
        event_dispatcher.cpp
    DEPS
        atlas_foundation
        atlas_platform
        atlas_serialization
)

# Winsock on Windows
if(WIN32)
    target_link_libraries(atlas_network PUBLIC ws2_32)
endif()
