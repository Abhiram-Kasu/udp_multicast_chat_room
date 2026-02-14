#pragma once
#include <boost/asio.hpp>
#include <cstdint>

using boost::asio::ip::udp;
struct Server {
private:
  int m_listening_address;
  uint32_t m_max_room_limit;
  boost::asio::io_context m_io_context;

public:
  Server(int address, uint32_t max_room_limit);
  ~Server();

  void serve();
};
