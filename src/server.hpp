#pragma once
#include "udp_server.hpp"
#include <boost/asio.hpp>
#include <boost/cobalt/channel.hpp>
#include <boost/cobalt/op.hpp>
#include <boost/cobalt/promise.hpp>

#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>

using boost::asio::ip::tcp;
using boost::asio::ip::udp;
using tcp_acceptor = boost::cobalt::use_op_t::as_default_on_t<tcp::acceptor>;
using tcp_socket = boost::cobalt::use_op_t::as_default_on_t<tcp::socket>;

template <class... Ts> struct overloads : Ts... {
  using Ts::operator()...;
};

template <typename Message> using Channel = boost::cobalt::channel<Message>;

struct TCPChatRoom {
  using connection_list = std::vector<std::weak_ptr<Channel<std::string>>>;
  std::mutex mutex{};
  std::string name;
  connection_list connections;
  uint64_t id;
  TCPChatRoom(const std::string &name, connection_list connections, uint64_t id)
      : name(name), connections(connections), id(id) {}
};

struct Server {
private:
  int m_listening_port;
  uint32_t m_max_room_limit;

  std::optional<tcp_acceptor> m_tcp_server_acceptor;
  std::optional<UDP_Server> m_udp_server;

  std::mutex chat_room_mutex;
  std::list<TCPChatRoom> chatRooms;

public:
  Server(int address, uint64_t max_room_limit);

  ~Server();

  boost::cobalt::promise<void> serve();

private:
  boost::cobalt::promise<
      std::variant<std::shared_ptr<Channel<std::string>>, std::string_view>>
  handle_sub(boost::json::object &);
  std::optional<std::shared_ptr<Channel<std::string>>>
  try_sub_to_room(uint64_t room_id);

  boost::cobalt::promise<
      std::variant<std::shared_ptr<Channel<std::string>>, std::string_view>>
  handle_create_and_sub(boost::json::object &);
  static boost::cobalt::promise<std::vector<char>> parse_data(tcp_socket &s);
  boost::cobalt::promise<void> accept_connections();
  boost::cobalt::promise<void> tcp_accept(tcp_socket s);
  void init_udp_server() {}
};
