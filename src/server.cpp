#include "server.hpp"
#include <boost/cobalt/this_coro.hpp>

#include <boost/json/src.hpp>
#include <format>
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
          std::visit(
              overloads{[&](std::shared_ptr<Channel<std::string>> channel) {
                          +([&]() -> boost::cobalt::promise<void> {
                            while (channel->is_open() && s.is_open()) {
                              auto data = co_await channel->read();
                              auto json_str =
                                  boost::json::serialize(boost::json::object{
                                      {"type", "message"}, {"data", data}});
                              co_await s.async_send(
                                  boost::asio::buffer(json_str));
                            }
                          }());
                        },
                        [&](std::string_view error) {
                          +([&]() -> boost::cobalt::promise<void> {
                            co_await s.async_send(boost::asio::buffer(
                                std::format("Failed: {}", error)));
                          }());
                        }},
              co_await handle_sub(structured_data));
        } else if (value == "desub") {
          co_return;
        } else if (value == "csub") {

          std::visit(
              overloads{
                  [](std::shared_ptr<Channel<std::string>> chan) {

                  },
                  [&](std::string_view error) {
                    +([&]() -> boost::cobalt::promise<void> {
                      co_await s.async_send(boost::asio::buffer(std::format(
                          "Failed to create and subscribe: {}", error)));
                    }());
                  }},
              co_await handle_create_and_sub(structured_data));
        } else [[likely]] if (value == "message") {
        } else {
          std::println("Invalid JSON");
          co_await s.async_send(boost::asio::buffer("Invalid JSON"));
        }
      }
    }
  }
}

boost::cobalt::promise<
    std::variant<std::shared_ptr<Channel<std::string>>, std::string_view>>
Server::handle_sub(boost::json::object &structured_data) {
  // get code of the subbed group
  if (not structured_data.contains("grp")) {
    co_return "grp not found";
  }

  const auto grp_res = [&] -> std::optional<uint64_t> {
    if (auto grp_result_uint64_t = structured_data.at("grp").try_as_uint64()) {
      return *grp_result_uint64_t;
    }
    if (auto grp_result_int64_t = structured_data.at("grp").try_as_int64()) {
      return static_cast<uint64_t>(*grp_result_int64_t);
    }

    return std::optional<uint64_t>{};
  }();

  if (not grp_res) {
    co_return "grp not valid";
  }
  auto grp = *grp_res;

  if (auto chan = try_sub_to_room(grp); chan.has_value()) {
    co_return chan.value();
  } else {
    co_return "No such group exists";
  }
}

std::optional<std::shared_ptr<Channel<std::string>>>
Server::try_sub_to_room(uint64_t room_id) {
  if (auto chat_room = std::ranges::find_if(
          chatRooms, [&](const auto &room) { return room.id == room_id; });
      chat_room != chatRooms.end()) {
    auto &chat_room_v = *chat_room;
    auto channel = std::make_shared<Channel<std::string>>(0u);

    std::lock_guard<std::mutex> guard{chat_room_v.mutex};
    chat_room_v.connections.push_back(channel);
    return channel;
  }
  return std::nullopt;
}

boost::cobalt::promise<
    std::variant<std::shared_ptr<Channel<std::string>>, std::string_view>>
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

  if (auto chan = try_sub_to_room(chat_room.id); chan.has_value()) {
    co_return chan.value();
  } else {
    co_return "Fatal error sub couldent be found";
  }
}
