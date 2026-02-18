#pragma once
#include "udp_server.hpp"
#include <boost/asio.hpp>
#include <boost/asio/experimental/channel.hpp>

#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>

using boost::asio::ip::tcp;
using boost::asio::ip::udp;

template <class... Ts> struct overloads : Ts... {
  using Ts::operator()...;
};

template <typename Message>
using Channel = boost::asio::experimental::channel<void(
    boost::system::error_code, Message)>;

struct TCPChatRoom {
  std::mutex mutex;
  std::vector<std::weak_ptr<Channel<std::string>>> connections;
  uint64_t id;
};

struct Server {
private:
  int m_listening_address;
  uint32_t m_max_room_limit;
  boost::asio::io_context m_io_context;

  tcp::acceptor m_tcp_server_acceptor;
  std::optional<UDP_Server> m_udp_server;

  std::list<TCPChatRoom> chatRooms;

public:
  Server(int address, uint64_t max_room_limit);

  ~Server();

  void serve();

private:
  boost::asio::awaitable<
      std::variant<std::shared_ptr<Channel<std::string>>, std::string_view>>
  handle_sub(boost::json::object &);
  static boost::asio::awaitable<std::vector<char>> parse_data(tcp::socket &s);
  boost::asio::awaitable<void> accept_connections();
  boost::asio::awaitable<void> tcp_accept(boost::asio::ip::tcp::socket s);
  void init_udp_server() {}
};
