cmake_minimum_required(VERSION 3.5)
set(CMAKE_CXX_STANDARD 26)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(udp_multicast_chat_room)

set(SRC_LIST
    src/main.cpp
    src/server.cpp
    src/server.hpp
    src/udp_server.cpp
    src/udp_server.hpp
)

add_executable(udp_multicast_chat_room ${SRC_LIST})

target_include_directories(udp_multicast_chat_room PRIVATE /opt/homebrew/include)
target_link_directories(udp_multicast_chat_room PRIVATE /opt/homebrew/lib)
target_link_libraries(udp_multicast_chat_room PRIVATE boost_json)