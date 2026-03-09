#pragma once
#include "tcp_chat_room.hpp"
#include "udp_server.hpp"
#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

using boost::asio::ip::tcp;
using boost::asio::ip::udp;
using tcp_acceptor =
    boost::asio::use_awaitable_t<>::as_default_on_t<tcp::acceptor>;
using tcp_socket = boost::asio::use_awaitable_t<>::as_default_on_t<tcp::socket>;

template <class... Ts> struct overloads : Ts... {
  using Ts::operator()...;
};

struct Server {
private:
  int m_listening_port;
  uint32_t m_max_room_limit;

  std::optional<tcp_acceptor> m_tcp_server_acceptor;
  std::optional<UDP_Server> m_udp_server;

  std::mutex chat_room_mutex;
  std::list<TCPChatRoom> chat_rooms;

public:
  Server(int address, uint64_t max_room_limit);

  ~Server();

  /// Sets up the io_context, spawns the async server logic onto it,
  /// and runs it across a thread pool. Blocks until shutdown.
  void serve(
      size_t thread_count = std::max(1u, std::thread::hardware_concurrency()));

private:
  using subscription = std::pair<std::shared_ptr<Channel<std::string>>,
                                 std::shared_ptr<Channel<std::string>>>;
  boost::asio::awaitable<std::variant<
      std::pair<std::shared_ptr<Channel<std::string>>, TCPChatRoom *>,
      std::string_view>>
  handle_sub(boost::json::object &,
             const std::map<TCPChatRoom *, subscription> &listener_map);
  std::shared_ptr<Channel<std::string>>
  sub_to_room(TCPChatRoom &room_id, boost::asio::any_io_executor exec);

  boost::asio::awaitable<void>
  spawn_listener(tcp_socket &socket, std::weak_ptr<Channel<std::string>> chan,
                 TCPChatRoom *chat_room);

  std::optional<TCPChatRoom *> try_find_room(uint64_t room_id);

  boost::asio::awaitable<std::variant<
      std::pair<std::shared_ptr<Channel<std::string>>, TCPChatRoom *>,
      std::string_view>>
  handle_create_and_sub(boost::json::object &);
  static boost::asio::awaitable<std::vector<char>> parse_data(tcp_socket &s);
  using Message = std::pair<std::string_view, TCPChatRoom *>;
  auto parse_message(boost::json::object &data)
      -> std::variant<Message, std::string_view>;

  auto parse_id(boost::json::object &data, std::string_view key)
      -> std::optional<uint64_t>;
  boost::asio::awaitable<void> serve_async();
  boost::asio::awaitable<void> accept_connections();
  boost::asio::awaitable<void> tcp_accept(tcp_socket s);
  void init_udp_server() {}
};
