# atlas_network — Sockets, EventDispatcher, channels, messages, NetworkInterface

atlas_add_library(atlas_network
    SOURCES
        address.cc
        bg_task_manager.cc
        socket.cc
        event_dispatcher.cc
        interface_table.cc
        bundle.cc
        channel.cc
        compression_filter.cc
        network_interface.cc
        reliable_udp.cc
        tcp_channel.cc
        udp_channel.cc
    DEPS
        atlas_foundation
        atlas_platform
        atlas_serialization
        atlas_zlib
)

# Winsock on Windows
if(WIN32)
    target_link_libraries(atlas_network PUBLIC ws2_32)
endif()
