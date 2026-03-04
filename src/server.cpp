#include "server.hpp"
#include "boost/cobalt/io/signal_set.hpp"
#include "boost/cobalt/promise.hpp"
#include "boost/cobalt/race.hpp"
#include "boost/cobalt/spawn.hpp"
#include "boost/cobalt/task.hpp"
#include "boost/cobalt/wait_group.hpp"
#include "boost/json/impl/serialize.ipp"
#include <algorithm>
#include <array>
#include <boost/cobalt/this_coro.hpp>
#include <boost/json/src.hpp>
#include <boost/system/system_error.hpp>
#include <csignal>
#include <exception>
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

void Server::serve(size_t thread_count) {
  std::println("Server::serve starting on port {} with {} threads",
               m_listening_port, thread_count);

  auto io_context = boost::asio::io_context{};

  boost::cobalt::spawn(io_context, serve_async(), [](std::exception_ptr e) {
    if (e) {
      try {
        std::rethrow_exception(e);
      } catch (const std::exception &ex) {
        std::println("server error: {}", ex.what());
      }
    }
  });

  std::vector<std::jthread> threads;
  threads.reserve(thread_count - 1);
  for (auto i{1uz}; i < thread_count; ++i) {
    threads.emplace_back([&io_context, i]() {
      std::println("worker thread {} started", i);
      io_context.run();
      std::println("worker thread {} finished", i);
    });
  }

  std::println("main thread entering io_context::run()");
  io_context.run();
  std::println("main thread exiting");
}

boost::cobalt::task<void> Server::serve_async() {
  std::println("Server::serve_async starting on port {}", m_listening_port);
  auto exec = co_await boost::cobalt::this_coro::executor;

  m_tcp_server_acceptor.emplace(exec);
  auto &acceptor = *m_tcp_server_acceptor;
  acceptor.open(tcp::v4());
  acceptor.set_option(boost::asio::socket_base::reuse_address(true));
  acceptor.bind(tcp::endpoint(tcp::v4(), m_listening_port));
  acceptor.listen();

  std::println("Server::serve_async acceptor ready");
  std::println("listening on {}", m_listening_port);

  boost::cobalt::io::signal_set signals({SIGINT, SIGTERM}, exec);

  auto shutdown = [&]() -> boost::cobalt::promise<void> {
    std::println("Server::serve_async waiting for shutdown signal");
    auto sig = co_await signals.wait();
    std::println("Server::serve_async shutdown signal received ({})", sig);
    if (m_tcp_server_acceptor) {
      boost::system::error_code ec;
      m_tcp_server_acceptor->close(ec);
      if (ec) {
        std::println("Server::serve_async acceptor close error: {}",
                     ec.message());
      } else {
        std::println("Server::serve_async acceptor closed");
      }
    }
    co_return;
  };

  std::println("Server::serve_async entering accept loop");
  co_await boost::cobalt::race(accept_connections(), shutdown());
  std::println("Server::serve_async accept loop exited");
}

boost::cobalt::promise<void> Server::accept_connections() {
  if (!m_tcp_server_acceptor) {
    std::println("accept_connections: no acceptor, returning");
    co_return;
  }
  auto &acceptor = *m_tcp_server_acceptor;
  std::list<boost::cobalt::promise<void>> sessions;
  for (;;) {
    std::println("accept_connections: waiting for connection");
    try {
      // clean up finished sessions
      sessions.remove_if(
          [](boost::cobalt::promise<void> &p) { return p.ready(); });

      auto socket = co_await acceptor.async_accept();
      std::println(
          "accept_connections: received connection (total active sessions: {})",
          sessions.size() + 1);
      sessions.push_back(tcp_accept(std::move(socket)));
    } catch (const boost::system::system_error &err) {
      std::println("accept_connections: accept error: {}", err.what());
      co_return;
    }
  }
}

boost::cobalt::promise<std::vector<char>> Server::parse_data(tcp_socket &s) {
  std::println("parse_data: waiting for newline-delimited payload");
  auto mutable_buffer = boost::asio::streambuf();

  auto bytes_read = co_await boost::asio::async_read_until(
      s, mutable_buffer, '\n', boost::cobalt::use_op);
  std::println("parse_data: read {} bytes, streambuf size = {}", bytes_read,
               mutable_buffer.size());

  std::istream response_stream(&mutable_buffer);
  std::string data_str(bytes_read, '\0');
  response_stream.read(data_str.data(), bytes_read);

  // hex dump of raw bytes before trimming
  std::print("parse_data: raw hex dump ({} bytes): ", data_str.size());
  for (unsigned char c : data_str) {
    std::print("{:02x} ", c);
  }
  std::println("");
  std::println("parse_data: raw string repr: [{}]", data_str);

  while (!data_str.empty() &&
         (data_str.back() == '\n' || data_str.back() == '\r')) {
    std::println("parse_data: trimming trailing byte 0x{:02x}",
                 static_cast<unsigned char>(data_str.back()));
    data_str.pop_back();
  }

  // hex dump after trimming
  std::print("parse_data: trimmed hex dump ({} bytes): ", data_str.size());
  for (unsigned char c : data_str) {
    std::print("{:02x} ", c);
  }
  std::println("");
  std::println("parse_data: trimmed string repr: [{}]", data_str);

  std::vector<char> data(data_str.begin(), data_str.end());
  std::println("parse_data: returning {} bytes", data.size());
  co_return data;
}

boost::cobalt::promise<void> Server::tcp_accept(tcp_socket s) {
  // parse until the newline delimiter to read a complete JSON message
  // incoming
  //
  std::println("tcp_accept: session started");
  auto current_subscriptions = std::map<TCPChatRoom *, subscription>{};
  for (;;) {
    try {
      std::println("tcp_accept: waiting for next message");
      const auto data = co_await parse_data(s);
      std::println("tcp_accept: received raw data ({} bytes): {}", data.size(),
                   std::string_view{data.begin(), data.end()});

      if (data.empty()) {
        std::println("tcp_accept: received empty line, ignoring");
        continue;
      }

      std::string_view data_str{data.begin(), data.end()};

      // hex dump of what we're about to parse
      std::print("tcp_accept: about to parse JSON hex ({} bytes): ",
                 data_str.size());
      for (unsigned char c : data_str) {
        std::print("{:02x} ", c);
      }
      std::println("");
      std::println("tcp_accept: about to parse JSON string: [{}]", data_str);

      boost::system::error_code error_code;
      auto parsed = boost::json::parse(data_str, error_code);
      if (error_code.failed()) {
        std::println("tcp_accept: failed to parse JSON: {} (error code: {})",
                     error_code.message(), error_code.value());
        co_await s.async_send(boost::asio::buffer("Failed to parse JSON: " +
                                                  error_code.message()));
        continue;
      }
      if (!parsed.is_object()) {
        std::println("tcp_accept: invalid JSON (not an object)");
        co_await s.async_send(boost::asio::buffer("Invalid JSON"));
        continue;
      }

      auto structured_data = parsed.as_object();

      if (structured_data.contains("type")) {
        if (auto type = structured_data.at("type").try_as_string()) {
          // if the type
          const std::string &value = type.value().c_str();
          std::println("tcp_accept: message type={}", value);

          if (value == "sub") {
            std::println("tcp_accept: handling sub request");
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
                            std::println(
                                "tcp_accept: subscribed to room id={} name={}",
                                chat_room->id, chat_room->name);
                          },
                          [&](std::string_view error) {
                            std::println("tcp_accept: sub failed: {}", error);
                            +([&]() -> boost::cobalt::promise<void> {
                              co_await s.async_send(boost::asio::buffer(
                                  std::format("Failed: {}", error)));
                            }());
                          }},
                co_await handle_sub(structured_data, current_subscriptions));
          } else if (value == "desub") {
            std::println("tcp_accept: desub request received, ending session");
            co_return;
          } else if (value == "csub") {
            std::println("tcp_accept: handling create+sub request");
            std::visit(
                overloads{
                    [&](std::pair<std::shared_ptr<Channel<std::string>>,
                                  TCPChatRoom *>
                            res) {
                      auto &[channel, chat_room] = res;
                      current_subscriptions.emplace(
                          chat_room,
                          std::make_pair(spawn_listener(s, channel, chat_room),
                                         std::move(channel)));
                      std::println(
                          "tcp_accept: created+subscribed room id={} name={}",
                          chat_room->id, chat_room->name);
                    },
                    [&](std::string_view error) {
                      std::println("tcp_accept: create+sub failed: {}", error);
                      auto res = ([&]() -> boost::cobalt::promise<void> {
                        co_await s.async_send(boost::asio::buffer(std::format(
                            "Failed to create and subscribe: {}", error)));
                      }());
                    }},
                co_await handle_create_and_sub(structured_data));

          } else [[likely]] if (value == "msg") {
            std::println("tcp_accept: handling message request");
            co_await std::visit(
                overloads{
                    [&](Message &&m) -> boost::cobalt::promise<void> {
                      auto &&[message, chat_room] = m;
                      std::lock_guard guard{chat_room->mutex};

                      // make sure that we have a subscription to the
                      // chat_room
                      if (not current_subscriptions.contains(chat_room)) {
                        std::println(
                            "tcp_accept: message rejected (not subscribed)");
                        co_await s.async_send(boost::asio::buffer(
                            "not subscriped to this room\n"));
                        co_return;
                      }

                      auto &curr_chan =
                          current_subscriptions.at(chat_room).second;

                      auto recipient_count = std::ranges::count_if(
                          chat_room->connections,
                          [&](std::weak_ptr<Channel<std::string>> chan) {
                            return chan.lock() != curr_chan;
                          });
                      std::println(
                          "tcp_accept: broadcasting message ({} bytes) to {} "
                          "recipients in room id={} name={}",
                          message.size(), recipient_count, chat_room->id,
                          chat_room->name);

                      auto subscriptions =
                          chat_room->connections |
                          std::views::filter(
                              [&](std::weak_ptr<Channel<std::string>> chan) {
                                return chan.lock() != curr_chan;
                              });

                      auto wg = boost::cobalt::wait_group{};

                      for (const auto connection : subscriptions) {
                        auto res = boost::json::serialize(boost::json::object{
                            {"grp", chat_room->name}, {"message", message}});

                        wg.push_back([&]() -> boost::cobalt::promise<void> {
                          co_await connection.lock()->write(
                              std::string{res.c_str()});
                        }());
                      }

                      co_await wg;
                      std::println("tcp_accept: broadcast complete");
                    },
                    [&](std::string_view error)
                        -> boost::cobalt::promise<void> {
                      std::println("tcp_accept: message parse error: {}",
                                   error);
                      co_await s.async_send(boost::asio::buffer(
                          std::format("Failed to send msg: {}", error)));
                    }},
                parse_message(structured_data));

          } else {
            std::println("tcp_accept: Invalid JSON (unknown type)");
            co_await s.async_send(boost::asio::buffer("Invalid JSON"));
          }
        } else {
          std::println("tcp_accept: type field is not a string");
          co_await s.async_send(boost::asio::buffer("Invalid JSON"));
        }
      } else {
        std::println("tcp_accept: missing type field");
        co_await s.async_send(boost::asio::buffer("Invalid JSON"));
      }
    } catch (const boost::system::system_error &err) {
      if (err.code() == boost::asio::error::eof ||
          err.code() == boost::asio::error::connection_reset ||
          err.code() == boost::asio::error::broken_pipe) {
        std::println("tcp_accept: client disconnected");
      } else {
        std::println("tcp_accept: error: {}", err.code().message());
      }
      co_return;
    } catch (const std::exception &err) {
      std::println("tcp_accept: unexpected error: {}", err.what());
      co_return;
    }
  }
}

boost::cobalt::promise<std::variant<
    std::pair<std::shared_ptr<Channel<std::string>>, TCPChatRoom *>,
    std::string_view>>
Server::handle_sub(boost::json::object &structured_data,
                   const std::map<TCPChatRoom *, subscription> &listener_map) {
  std::println("handle_sub: processing subscription request");
  // get code of the subbed group
  if (not structured_data.contains("grp")) {
    std::println("handle_sub: missing grp field");
    co_return "grp not found";
  }

  const auto grp_res = parse_id(structured_data, "grp");

  if (not grp_res) {
    std::println("handle_sub: invalid grp id");
    co_return "grp not valid";
  }
  auto grp = *grp_res;
  std::println("handle_sub: parsed grp id={}", grp);

  // check if listener already exists for this group
  if (std::ranges::any_of(listener_map, [=](const auto &kv) {
        const auto &[k, _] = kv;
        return k->id == grp;
      })) {
    std::println("handle_sub: already subscribed to grp id={}", grp);
    co_return "Already subscribed to this Chat Room";
  }

  if (auto chat_room_res = try_find_room(grp)) {
    auto *chat_room = *chat_room_res;
    std::println("handle_sub: found room id={} name={}", chat_room->id,
                 chat_room->name);
    co_return std::pair{sub_to_room(*chat_room), chat_room};
  } else {
    std::println("handle_sub: no such group id={}", grp);
    co_return "No such group exists";
  }
}

std::shared_ptr<Channel<std::string>>
Server::sub_to_room(TCPChatRoom &chat_room) {
  std::println("sub_to_room: subscribing to room id={} name={}", chat_room.id,
               chat_room.name);
  auto channel = std::make_shared<Channel<std::string>>(0u);
  std::lock_guard<std::mutex> guard{chat_room.mutex};
  chat_room.connections.push_back(channel);
  std::println("sub_to_room: room id={} now has {} connections", chat_room.id,
               chat_room.connections.size());
  return channel;
}

boost::cobalt::promise<std::variant<
    std::pair<std::shared_ptr<Channel<std::string>>, TCPChatRoom *>,
    std::string_view>>
Server::handle_create_and_sub(boost::json::object &data) {
  std::println("handle_create_and_sub: processing create+sub request");

  if (not data.contains("grp_name")) {
    std::println("handle_create_and_sub: missing grp_name");
    co_return "'grp_name' not found";
  }
  if (not data.at("grp_name").is_string()) {
    std::println("handle_create_and_sub: grp_name invalid format");
    co_return "'grp_name' in invalid format";
  }

  auto &chat_room = [&] -> decltype(auto) {
    const auto &grp_name = std::string_view{data.at("grp_name").as_string()};

    std::lock_guard<std::mutex> guard{this->chat_room_mutex};
    auto &room = this->chatRooms.emplace_back(std::string(grp_name),
                                              TCPChatRoom::connection_list{},
                                              chatRooms.size() + 1);
    std::println("handle_create_and_sub: created room id={} name={}", room.id,
                 room.name);
    return room;
  }();

  co_return std::pair{sub_to_room(chat_room), &chat_room};
}

std::optional<TCPChatRoom *> Server::try_find_room(uint64_t room_id) {
  std::println("try_find_room: searching for room id={}", room_id);
  std::lock_guard<std::mutex> mut{chat_room_mutex};
  if (auto chat_room = std::ranges::find_if(
          chatRooms, [&](const auto &room) { return room.id == room_id; });
      chat_room != chatRooms.end()) {
    std::println("try_find_room: found room id={} name={}", chat_room->id,
                 chat_room->name);
    return std::addressof(*chat_room);
  }
  std::println("try_find_room: room id={} not found", room_id);
  return std::nullopt;
}

boost::cobalt::promise<void>
Server::spawn_listener(tcp_socket &socket,
                       std::weak_ptr<Channel<std::string>> chan,
                       TCPChatRoom *chat_room) {
  std::println("spawn_listener: started for room id={} name={}", chat_room->id,
               chat_room->name);
  while (not chan.expired() and chan.lock()->is_open() && socket.is_open()) {
    try {
      auto data = co_await chan.lock()->read();
      std::println("spawn_listener: room id={} sending message ({} bytes)",
                   chat_room->id, data.size());
      auto json_str = boost::json::serialize(boost::json::object{
          {"type", "message"}, {"data", data}, {"grp_name", chat_room->name}});
      co_await socket.async_send(boost::asio::buffer(json_str));
    } catch (const boost::system::system_error &err) {
      if (err.code() == boost::asio::error::eof ||
          err.code() == boost::asio::error::connection_reset ||
          err.code() == boost::asio::error::broken_pipe ||
          err.code() == boost::asio::error::operation_aborted ||
          err.code().value() == 89 /* ECANCELED */) {
        std::println("spawn_listener: client left room id={} name={}",
                     chat_room->id, chat_room->name);
      } else {
        std::println("spawn_listener: error for room id={} name={}: {}",
                     chat_room->id, chat_room->name, err.code().message());
      }
      break;
    } catch (const std::exception &err) {
      std::println(
          "spawn_listener: unexpected error for room id={} name={}: {}",
          chat_room->id, chat_room->name, err.what());
      break;
    }
  }
  std::println("spawn_listener: exiting for room id={} name={}", chat_room->id,
               chat_room->name);
}

auto Server::parse_message(boost::json::object &data)
    -> std::variant<Message, std::string_view> {
  std::println("parse_message: parsing incoming message");
  if (not data.contains("grp")) {
    std::println("parse_message: missing grp field");
    return "no grp found";
  }
  if (not data.contains("msg")) {
    std::println("parse_message: missing msg field");
    return "no msg found";
  }

  if (auto grp_res = parse_id(data, "grp")) {
    auto grp = *grp_res;
    if (auto room = try_find_room(grp)) {
      auto message = std::string_view{data.at("msg").as_string()};
      std::println("parse_message: parsed grp id={} msg bytes={}", grp,
                   message.size());
      return std::pair{message, *room};
    } else {
      std::println("parse_message: grp id={} not found", grp);
      return "grp not found";
    }
  } else {
    std::println("parse_message: invalid grp id");
    return "invalid grp";
  }
}

auto Server::parse_id(boost::json::object &data, std::string_view key)
    -> std::optional<uint64_t> {
  std::println("parse_id: parsing key '{}'", key);
  if (auto grp_result_uint64_t = data.at(key).try_as_uint64()) {
    std::println("parse_id: parsed {} as uint64 {}", key, *grp_result_uint64_t);
    return *grp_result_uint64_t;
  }
  if (auto grp_result_int64_t = data.at(key).try_as_int64()) {
    std::println("parse_id: parsed {} as int64 {}", key, *grp_result_int64_t);
    return static_cast<uint64_t>(*grp_result_int64_t);
  }

  std::println("parse_id: failed to parse key '{}'", key);
  return std::optional<uint64_t>{};
}
