#include "udp_server.hpp"
#include "boost/json/object.hpp"
#include "tcp_chat_room.hpp"
#include <mutex>

auto UDP_Server::handle_upgrade(TCPChatRoom &&socket) -> void {
  {

    // Create new UDPChatRoom first and then send the message for all listeners
    // to switch protocls to udp multicast.

    std::lock_guard guard{socket.mutex};
    for (auto &listener : socket.connections) {

      if (not listener.expired()) {

        // listener.lock()->write(boost::json::object { { "type", "upgrade" }, {
        // "id", socket.id }})
      }
    }
  }
}
auto UDP_Server::handle_sub() -> void {}
auto UDP_Server::handle_message() -> void {}
auto UDP_Server::handle_desub() -> void {}
