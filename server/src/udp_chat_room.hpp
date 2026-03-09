#pragma once

#include "tcp_chat_room.hpp"

struct UDPChatRoom {
private:
  UDPChatRoom();

public:
  static auto construct_from_tcp_chat_room(TCPChatRoom &&chat_room)
      -> UDPChatRoom;
};
