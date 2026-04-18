# atlas_network — sockets, event dispatcher, channels, messages

add_library(atlas_network STATIC
  address.cc
  bg_task_manager.cc
  bundle.cc
  channel.cc
  compression_filter.cc
  event_dispatcher.cc
  interface_table.cc
  network_interface.cc
  reliable_udp.cc
  socket.cc
  tcp_channel.cc
  udp_channel.cc
)

target_include_directories(atlas_network PUBLIC "${CMAKE_SOURCE_DIR}/src/lib")
target_link_libraries(atlas_network
  PUBLIC atlas_foundation atlas_platform atlas_serialization ZLIB::ZLIB atlas_compiler_options
)

if(WIN32)
  target_link_libraries(atlas_network PUBLIC ws2_32)
endif()
