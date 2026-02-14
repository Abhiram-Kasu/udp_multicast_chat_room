#include "server.hpp"

Server::Server(int address, uint32_t max_room_limit) {
  m_listening_address = address;
  m_max_room_limit = max_room_limit;
}
