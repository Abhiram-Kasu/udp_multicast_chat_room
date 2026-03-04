#include "server.hpp"
#include "boost/asio/registered_buffer.hpp"
#include "boost/cobalt/promise.hpp"
#include "boost/cobalt/this_thread.hpp"
#include "boost/cobalt/wait_group.hpp"
#include "boost/json/impl/serialize.ipp"
#include <algorithm>
#include <array>
#include <boost/cobalt/this_coro.hpp>

#include <boost/json/src.hpp>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

Server::Server(int address, uint64_t max_room_limit)
    : m_listening_port(address), m_max_room_limit(max_room_limit),
      m_tcp_server_acceptor(std::nullopt) {}

Server::~Server() = default;

boost::cobalt::promise<void> Server::serve() {
  auto exec = co_await boost::cobalt::this_coro::executor;
  m_tcp_server_acceptor.emplace(exec,
                                tcp::endpoint(tcp::v4(), m_listening_port));
  std::println("listening on {}", m_listening_port);
  co_await accept_connections();
}

boost::cobalt::promise<void> Server::accept_connections() {
  if (!m_tcp_server_acceptor) {
    co_return;
  }
  auto &acceptor = *m_tcp_server_acceptor;
  for (;;) {
    auto socket = co_await acceptor.async_accept();
    std::println("Received Connection");
    co_await tcp_accept(std::move(socket));
  }
}

boost::cobalt::promise<std::vector<char>> Server::parse_data(tcp_socket &s) {
  auto mutable_buffer = boost::asio::streambuf();

  auto bytes_read = co_await boost::asio::async_read_until(
      s, mutable_buffer, '\n', boost::cobalt::use_op);

  std::istream response_stream(&mutable_buffer);
  std::string data_str(bytes_read, '\0');
  response_stream.read(data_str.data(), bytes_read);

  if (!data_str.empty() && data_str.back() == '\n') {
    data_str.pop_back();
  }

  std::vector<char> data(data_str.begin(), data_str.end());
  co_return data;
}

boost::cobalt::promise<void> Server::tcp_accept(tcp_socket s) {
  // parse until the newline delimiter to read a complete JSON message
  // incoming
  //

  auto current_subscriptions = std::map<TCPChatRoom *, subscription>{};
  for (;;) {
    const auto data = co_await parse_data(s);
    std::println("Got data: {}", std::string_view{data.begin(), data.end()});
    std::string_view data_str{data.begin(), data.end()};
    boost::system::error_code error_code;
    auto structured_data = boost::json::parse(data_str, error_code).as_object();
    if (error_code.failed()) {
      std::println("Failed to parse JSON: {}", error_code.message());
      co_await s.async_send(
          boost::asio::buffer("Failed to parse JSON: " + error_code.message()));
    }
    if (structured_data.contains("type")) {
      if (auto type = structured_data.at("type").try_as_string()) {
        // if the type
        const std::string &value = type.value().c_str();

        if (value == "sub") {
          std::println("Found sub type, handling...");
          // check if we already have a listener

          std::visit(
              overloads{[&](std::pair<std::shared_ptr<Channel<std::string>>,
                                      TCPChatRoom *>
                                res) {
                          // update current subscriptions
                          //
                          auto &[channel, chat_room] = res;
                          current_subscriptions.emplace(
                              chat_room,
                              std::make_pair(
                                  spawn_listener(s, std::weak_ptr{channel},
                                                 chat_room),
                                  std::move(channel)));
                        },
                        [&](std::string_view error) {
                          +([&]() -> boost::cobalt::promise<void> {
                            co_await s.async_send(boost::asio::buffer(
                                std::format("Failed: {}", error)));
                          }());
                        }},
              co_await handle_sub(structured_data, current_subscriptions));
        } else if (value == "desub") {
          co_return;
        } else if (value == "csub") {
          std::visit(
              overloads{
                  [&](std::pair<std::shared_ptr<Channel<std::string>>,
                                TCPChatRoom *>
                          res) {
                    auto &[channel, chat_room] = res;
                    current_subscriptions.emplace(
                        chat_room, spawn_listener(s, channel, chat_room));
                  },
                  [&](std::string_view error) {
                    auto res = ([&]() -> boost::cobalt::promise<void> {
                      co_await s.async_send(boost::asio::buffer(std::format(
                          "Failed to create and subscribe: {}", error)));
                    }());
                  }},
              co_await handle_create_and_sub(structured_data));

        } else [[likely]] if (value == "message") {

          co_await std::visit(
              overloads{
                  [&](Message &&m) -> boost::cobalt::promise<void> {
                    auto &&[message, chat_room] = m;
                    std::lock_guard guard{chat_room->mutex};

                    // make sure that we have a subscription to the
                    // chat_room
                    if (not current_subscriptions.contains(chat_room)) {
                      co_await s.async_send(
                          boost::asio::buffer("not subscriped to this room\n"));
                      co_return;
                    }

                    auto &[_, curr_chan] = current_subscriptions[chat_room];

                    auto subscriptions =
                        chat_room->connections |
                        std::views::filter(
                            [&](std::weak_ptr<Channel<std::string>> chan) {
                              return chan.lock() != curr_chan;
                            });

                    auto wg = boost::cobalt::wait_group{};

                    for (const std::weak_ptr<Channel<std::string>> connection :
                         subscriptions) {
                      auto res = boost::json::serialize(boost::json::object{
                          {"grp", chat_room->name}, {"message", message}});

                      wg.push_back([&]() -> boost::cobalt::promise<void> {
                          co_await connection.lock()->write(std::string{res.c_str()}));
                      }());
                    }

                    co_await wg;
                  },
                  [&](std::string_view error) -> boost::cobalt::promise<void> {
                    s.async_send(boost::asio::buffer(
                        std::format("Failed to send msg: {}", error)));
                  }},
              parse_message(structured_data));

        } else {
          std::println("Invalid JSON");
          co_await s.async_send(boost::asio::buffer("Invalid JSON"));
        }
      }
    }
  }
}

boost::cobalt::promise<std::variant<
    std::pair<std::shared_ptr<Channel<std::string>>, TCPChatRoom *>,
    std::string_view>>
Server::handle_sub(boost::json::object &structured_data,
                   const std::map<TCPChatRoom *, subscription> &listener_map) {
  // get code of the subbed group
  if (not structured_data.contains("grp")) {
    co_return "grp not found";
  }

  const auto grp_res = parse_id(structured_data, "grp");

  if (not grp_res) {
    co_return "grp not valid";
  }
  auto grp = *grp_res;

  // check if listener already exists for this group
  if (std::ranges::any_of(listener_map, [=](const auto &kv) {
        const auto &[k, _] = kv;
        return k->id == grp;
      })) {
    co_return "Already subscribed to this Chat Room";
  }

  if (auto chat_room_res = try_find_room(grp)) {
    auto *chat_room = *chat_room_res;
    co_return std::pair{sub_to_room(*chat_room), chat_room};
  } else {
    co_return "No such group exists";
  }
}

std::shared_ptr<Channel<std::string>>
Server::sub_to_room(TCPChatRoom &chat_room) {
  auto channel = std::make_shared<Channel<std::string>>(0u);
  std::lock_guard<std::mutex> guard{chat_room.mutex};
  chat_room.connections.push_back(channel);
  return channel;
}

boost::cobalt::promise<std::variant<
    std::pair<std::shared_ptr<Channel<std::string>>, TCPChatRoom *>,
    std::string_view>>
Server::handle_create_and_sub(boost::json::object &data) {

  if (not data.contains("grp_name")) {
    co_return "'grp_name' not found";
  }
  if (not data.at("grp_name").is_string()) {
    co_return "'grp_name' in invalid format";
  }

  auto &chat_room = [&] -> decltype(auto) {
    const auto &grp_name = std::string_view{data.at("grp_name").as_string()};

    std::lock_guard<std::mutex> guard{this->chat_room_mutex};
    return this->chatRooms.emplace_back(std::string(grp_name),
                                        TCPChatRoom::connection_list{},
                                        chatRooms.size() + 1);
  }();

  co_return std::pair{sub_to_room(chat_room), &chat_room};
}

std::optional<TCPChatRoom *> Server::try_find_room(uint64_t room_id) {
  std::lock_guard<std::mutex> mut{chat_room_mutex};
  if (auto chat_room = std::ranges::find_if(
          chatRooms, [&](const auto &room) { return room.id == room_id; });
      chat_room != chatRooms.end()) {
    return std::addressof(*chat_room);
  }
  return std::nullopt;
}

boost::cobalt::promise<void>
Server::spawn_listener(tcp_socket &socket,
                       std::weak_ptr<Channel<std::string>> chan,
                       TCPChatRoom *chat_room) {
  while (not chan.expired() and chan.lock()->is_open() && socket.is_open()) {
    auto data = co_await chan.lock()->read();
    auto json_str = boost::json::serialize(boost::json::object{
        {"type", "message"}, {"data", data}, {"grp_name", chat_room->name}});
    co_await socket.async_send(boost::asio::buffer(json_str));
  }
}

auto Server::parse_message(boost::json::object &data)
    -> std::variant<Message, std::string_view> {
  if (not data.contains("grp")) {
    return "no grp found";
  }
  if (not data.contains("msg")) {
    return "no msg found";
  }

  if (auto grp_res = parse_id(data, "grp")) {
    auto grp = *grp_res;
    if (auto room = try_find_room(grp)) {
      auto message = std::string_view{data.at("msg").as_string()};
      return std::pair{message, *room};
    } else {
      return "grp not found";
    }
  } else {
    return "invalid grp";
  }
}

auto parse_id(boost::json::object &data, std::string_view key)
    -> std::optional<uint64_t> {
  if (auto grp_result_uint64_t = data.at("grp").try_as_uint64()) {
    return *grp_result_uint64_t;
  }
  if (auto grp_result_int64_t = data.at("grp").try_as_int64()) {
    return static_cast<uint64_t>(*grp_result_int64_t);
  }

  return std::optional<uint64_t>{};
}
