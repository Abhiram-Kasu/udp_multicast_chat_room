#pragma once

#include "tcp_chat_room.hpp"
#include "udp_chat_room.hpp"
#include <boost/asio.hpp>
#include <expected>
#include <list>
#include <mutex>

using boost::asio::ip::tcp;
using boost::asio::ip::udp;
using tcp_socket = boost::asio::use_awaitable_t<>::as_default_on_t<tcp::socket>;

struct UDP_Server {
private:
  std::mutex chat_room_mutex;
  std::list<UDPChatRoom> chat_rooms;

  auto create_and_add_chat_room()
      -> std::expected<UDPChatRoom *, std::string_view>;

public:
  auto handle_upgrade(TCPChatRoom &&socket) -> void;
  auto handle_sub() -> void;
  auto handle_message() -> void;
  auto handle_desub() -> void;
};
