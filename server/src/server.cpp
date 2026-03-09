#include "server.hpp"
#include "log.hpp"
#include <algorithm>
#include <array>
#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/json/src.hpp>
#include <boost/system/system_error.hpp>
#include <csignal>
#include <exception>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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
  println("Server::serve starting on port {} with {} threads", m_listening_port,
          thread_count);

  auto io_context = boost::asio::io_context{};

  auto work_guard = boost::asio::make_work_guard(io_context);

  boost::asio::co_spawn(
      io_context,
      [this, &work_guard]() -> boost::asio::awaitable<void> {
        co_await serve_async();
        work_guard.reset();
      },
      [](std::exception_ptr e) {
        if (e) {
          try {
            std::rethrow_exception(e);
          } catch (const std::exception &ex) {
            println("server error: {}", ex.what());
          }
        }
      });

  std::vector<std::jthread> threads;
  threads.reserve(thread_count - 1);
  for (size_t i = 1; i < thread_count; ++i) {
    threads.emplace_back([&io_context, i]() {
      println("worker thread {} started", i);
      io_context.run();
      println("worker thread {} finished", i);
    });
  }

  println("main thread entering io_context::run()");
  io_context.run();
  println("main thread exiting");
}

boost::asio::awaitable<void> Server::serve_async() {
  println("Server::serve_async starting on port {}", m_listening_port);
  auto exec = co_await boost::asio::this_coro::executor;

  m_tcp_server_acceptor.emplace(exec);
  auto &acceptor = *m_tcp_server_acceptor;
  acceptor.open(tcp::v4());
  acceptor.set_option(boost::asio::socket_base::reuse_address(true));
  acceptor.bind(tcp::endpoint(tcp::v4(), m_listening_port));
  acceptor.listen();

  println("Server::serve_async acceptor ready");
  println("listening on {}", m_listening_port);

  boost::asio::signal_set signals(exec, SIGINT, SIGTERM);

  bool accept_done = false;
  boost::asio::co_spawn(
      exec,
      [this]() -> boost::asio::awaitable<void> {
        co_await accept_connections();
      },
      [&accept_done](std::exception_ptr e) {
        accept_done = true;
        if (e) {
          try {
            std::rethrow_exception(e);
          } catch (const std::exception &ex) {
            println("accept_connections error: {}", ex.what());
          }
        }
      });

  println("Server::serve_async waiting for shutdown signal");
  boost::system::error_code ec;
  auto sig = co_await signals.async_wait(
      boost::asio::redirect_error(boost::asio::use_awaitable, ec));
  if (!ec) {
    println("Server::serve_async shutdown signal received ({})", sig);
  }

  if (m_tcp_server_acceptor) {
    boost::system::error_code close_ec;
    m_tcp_server_acceptor->close(close_ec);
    if (close_ec) {
      println("Server::serve_async acceptor close error: {}",
              close_ec.message());
    } else {
      println("Server::serve_async acceptor closed");
    }
  }

  println("Server::serve_async accept loop exited");
}

boost::asio::awaitable<void> Server::accept_connections() {
  if (not m_tcp_server_acceptor) {
    println("accept_connections: no acceptor, returning");
    co_return;
  }

  auto exec = co_await boost::asio::this_coro::executor;
  auto &acceptor = *m_tcp_server_acceptor;

  for (;;) {
    println("accept_connections: waiting for connection");
    try {
      auto socket = co_await acceptor.async_accept();
      println("accept_connections: received connection");

      boost::asio::co_spawn(
          exec,
          [this,
           s = std::move(socket)]() mutable -> boost::asio::awaitable<void> {
            co_await tcp_accept(std::move(s));
          },
          [](std::exception_ptr e) {
            if (e) {
              try {
                std::rethrow_exception(e);
              } catch (const std::exception &ex) {
                println("tcp_accept session error: {}", ex.what());
              }
            }
          });
    } catch (const boost::system::system_error &err) {
      println("accept_connections: accept error: {}", err.what());
      co_return;
    }
  }
}

boost::asio::awaitable<std::vector<char>> Server::parse_data(tcp_socket &s) {
  println("parse_data: waiting for newline-delimited payload");
  auto mutable_buffer = boost::asio::streambuf();

  auto bytes_read = co_await boost::asio::async_read_until(
      s, mutable_buffer, '\n', boost::asio::use_awaitable);
  println("parse_data: read {} bytes, streambuf size = {}", bytes_read,
          mutable_buffer.size());

  std::istream response_stream(&mutable_buffer);
  std::string data_str(bytes_read, '\0');
  response_stream.read(data_str.data(), bytes_read);

  print("parse_data: raw hex dump ({} bytes): ", data_str.size());
  for (unsigned char c : data_str) {
    print("{:02x} ", c);
  }
  println("");
  println("parse_data: raw string repr: [{}]", data_str);

  while (!data_str.empty() &&
         (data_str.back() == '\n' || data_str.back() == '\r')) {
    println("parse_data: trimming trailing byte 0x{:02x}",
            static_cast<unsigned char>(data_str.back()));
    data_str.pop_back();
  }

  print("parse_data: trimmed hex dump ({} bytes): ", data_str.size());
  for (unsigned char c : data_str) {
    print("{:02x} ", c);
  }
  println("");
  println("parse_data: trimmed string repr: [{}]", data_str);

  std::vector<char> data(data_str.begin(), data_str.end());
  println("parse_data: returning {} bytes", data.size());
  co_return data;
}

boost::asio::awaitable<void> Server::tcp_accept(tcp_socket s) {
  println("tcp_accept: session started");
  auto exec = co_await boost::asio::this_coro::executor;
  auto current_subscriptions = std::map<TCPChatRoom *, subscription>{};
  for (;;) {
    try {
      println("tcp_accept: waiting for next message");
      const auto data = co_await parse_data(s);
      println("tcp_accept: received raw data ({} bytes): {}", data.size(),
              std::string_view{data.begin(), data.end()});

      if (data.empty()) {
        println("tcp_accept: received empty line, ignoring");
        continue;
      }

      std::string_view data_str{data.begin(), data.end()};

      print("tcp_accept: about to parse JSON hex ({} bytes): ",
            data_str.size());
      for (unsigned char c : data_str) {
        print("{:02x} ", c);
      }
      println("");
      println("tcp_accept: about to parse JSON string: [{}]", data_str);

      boost::system::error_code error_code;
      auto parsed = boost::json::parse(data_str, error_code);
      if (error_code.failed()) {
        println("tcp_accept: failed to parse JSON: {} (error code: {})",
                error_code.message(), error_code.value());
        co_await s.async_send(boost::asio::buffer("Failed to parse JSON: " +
                                                  error_code.message()));
        continue;
      }
      if (!parsed.is_object()) {
        println("tcp_accept: invalid JSON (not an object)");
        co_await s.async_send(boost::asio::buffer("Invalid JSON"));
        continue;
      }

      auto structured_data = parsed.as_object();

      if (structured_data.contains("type")) {
        auto type_val = structured_data.at("type");
        if (type_val.is_string()) {
          const std::string value{type_val.as_string().c_str()};
          println("tcp_accept: message type={}", value);

          if (value == "sub") {
            println("tcp_accept: handling sub request");

            auto result =
                co_await handle_sub(structured_data, current_subscriptions);
            if (auto *pair_ptr =
                    std::get_if<std::pair<std::shared_ptr<Channel<std::string>>,
                                          TCPChatRoom *>>(&result)) {
              auto &[channel, chat_room] = *pair_ptr;

              auto weak_chan = std::weak_ptr<Channel<std::string>>{channel};
              auto *cr = chat_room;
              boost::asio::co_spawn(
                  exec,
                  [this, &s, weak_chan, cr]() -> boost::asio::awaitable<void> {
                    co_await spawn_listener(s, weak_chan, cr);
                  },
                  boost::asio::detached);

              current_subscriptions.emplace(
                  chat_room, std::make_pair(channel, std::move(channel)));
              println("tcp_accept: subscribed to room id={} name={}",
                      chat_room->id, chat_room->name);
            } else {
              auto error = std::get<std::string_view>(result);
              println("tcp_accept: sub failed: {}", error);
              co_await s.async_send(
                  boost::asio::buffer(std::format("Failed: {}", error)));
            }
          } else if (value == "desub") {
            println("tcp_accept: desub request received, ending session");
            for (auto &[room, sub] : current_subscriptions) {
              sub.first->close();
            }
            co_return;
          } else if (value == "csub") {
            println("tcp_accept: handling create+sub request");
            auto result = co_await handle_create_and_sub(structured_data);
            if (auto *pair_ptr =
                    std::get_if<std::pair<std::shared_ptr<Channel<std::string>>,
                                          TCPChatRoom *>>(&result)) {
              auto &[channel, chat_room] = *pair_ptr;

              auto weak_chan = std::weak_ptr<Channel<std::string>>{channel};
              auto *cr = chat_room;
              boost::asio::co_spawn(
                  exec,
                  [this, &s, weak_chan, cr]() -> boost::asio::awaitable<void> {
                    co_await spawn_listener(s, weak_chan, cr);
                  },
                  boost::asio::detached);

              current_subscriptions.emplace(
                  chat_room, std::make_pair(channel, std::move(channel)));
              println("tcp_accept: created+subscribed room id={} name={}",
                      chat_room->id, chat_room->name);
            } else {
              auto error = std::get<std::string_view>(result);
              println("tcp_accept: create+sub failed: {}", error);
              co_await s.async_send(boost::asio::buffer(
                  std::format("Failed to create and subscribe: {}", error)));
            }

          } else if (value == "msg") {
            println("tcp_accept: handling message request");

            auto msg_result = parse_message(structured_data);
            if (auto *msg_ptr = std::get_if<Message>(&msg_result)) {
              auto &&[message, chat_room] = *msg_ptr;
              std::lock_guard guard{chat_room->mutex};

              if (not current_subscriptions.contains(chat_room)) {
                println("tcp_accept: message rejected (not subscribed)");
                co_await s.async_send(
                    boost::asio::buffer("not subscribed to this room\n"));
                continue;
              }

              auto &curr_chan = current_subscriptions.at(chat_room).second;

              auto recipient_count = std::ranges::count_if(
                  chat_room->connections,
                  [&](std::weak_ptr<Channel<std::string>> chan) {
                    return chan.lock() != curr_chan;
                  });
              println("tcp_accept: broadcasting message ({} bytes) to {} "
                      "recipients in room id={} name={}",
                      message.size(), recipient_count, chat_room->id,
                      chat_room->name);

              for (const auto &connection : chat_room->connections) {
                auto locked = connection.lock();
                if (locked && locked != curr_chan) {
                  auto res = boost::json::serialize(boost::json::object{
                      {"grp", chat_room->name}, {"message", message}});
                  locked->write(std::string{res.c_str()});
                }
              }

              println("tcp_accept: broadcast complete");
            } else {
              auto error = std::get<std::string_view>(msg_result);
              println("tcp_accept: message parse error: {}", error);
              co_await s.async_send(boost::asio::buffer(
                  std::format("Failed to send msg: {}", error)));
            }

          } else {
            println("tcp_accept: Invalid JSON (unknown type)");
            co_await s.async_send(boost::asio::buffer("Invalid JSON"));
          }
        } else {
          println("tcp_accept: type field is not a string");
          co_await s.async_send(boost::asio::buffer("Invalid JSON"));
        }
      } else {
        println("tcp_accept: missing type field");
        co_await s.async_send(boost::asio::buffer("Invalid JSON"));
      }
    } catch (const boost::system::system_error &err) {
      if (err.code() == boost::asio::error::eof ||
          err.code() == boost::asio::error::connection_reset ||
          err.code() == boost::asio::error::broken_pipe) {
        println("tcp_accept: client disconnected");
      } else {
        println("tcp_accept: error: {}", err.code().message());
      }
      for (auto &[room, sub] : current_subscriptions) {
        sub.first->close();
      }
      co_return;
    } catch (const std::exception &err) {
      println("tcp_accept: unexpected error: {}", err.what());
      for (auto &[room, sub] : current_subscriptions) {
        sub.first->close();
      }
      co_return;
    }
  }
}

boost::asio::awaitable<std::variant<
    std::pair<std::shared_ptr<Channel<std::string>>, TCPChatRoom *>,
    std::string_view>>
Server::handle_sub(boost::json::object &structured_data,
                   const std::map<TCPChatRoom *, subscription> &listener_map) {
  println("handle_sub: processing subscription request");
  auto exec = co_await boost::asio::this_coro::executor;

  if (not structured_data.contains("grp")) {
    println("handle_sub: missing grp field");
    co_return std::string_view{"grp not found"};
  }

  const auto grp_res = parse_id(structured_data, "grp");

  if (not grp_res) {
    println("handle_sub: invalid grp id");
    co_return std::string_view{"grp not valid"};
  }
  auto grp = *grp_res;
  println("handle_sub: parsed grp id={}", grp);

  if (std::ranges::any_of(listener_map, [=](const auto &kv) {
        const auto &[k, _] = kv;
        return k->id == grp;
      })) {
    println("handle_sub: already subscribed to grp id={}", grp);
    co_return std::string_view{"Already subscribed to this Chat Room"};
  }

  if (auto chat_room_res = try_find_room(grp)) {
    auto *chat_room = *chat_room_res;
    println("handle_sub: found room id={} name={}", chat_room->id,
            chat_room->name);
    co_return std::pair{sub_to_room(*chat_room, exec), chat_room};
  } else {
    println("handle_sub: no such group id={}", grp);
    co_return std::string_view{"No such group exists"};
  }
}

std::shared_ptr<Channel<std::string>>
Server::sub_to_room(TCPChatRoom &chat_room, boost::asio::any_io_executor exec) {
  println("sub_to_room: subscribing to room id={} name={}", chat_room.id,
          chat_room.name);
  auto channel = std::make_shared<Channel<std::string>>(exec);
  std::lock_guard<std::mutex> guard{chat_room.mutex};
  chat_room.connections.push_back(channel);
  println("sub_to_room: room id={} now has {} connections", chat_room.id,
          chat_room.connections.size());
  return channel;
}

boost::asio::awaitable<std::variant<
    std::pair<std::shared_ptr<Channel<std::string>>, TCPChatRoom *>,
    std::string_view>>
Server::handle_create_and_sub(boost::json::object &data) {
  println("handle_create_and_sub: processing create+sub request");
  auto exec = co_await boost::asio::this_coro::executor;

  if (not data.contains("grp_name")) {
    println("handle_create_and_sub: missing grp_name");
    co_return std::string_view{"'grp_name' not found"};
  }
  if (not data.at("grp_name").is_string()) {
    println("handle_create_and_sub: grp_name invalid format");
    co_return std::string_view{"'grp_name' in invalid format"};
  }

  auto &chat_room = [&]() -> decltype(auto) {
    const auto grp_name = std::string_view{data.at("grp_name").as_string()};

    std::lock_guard<std::mutex> guard{this->chat_room_mutex};
    auto &room = this->chat_rooms.emplace_back(std::string(grp_name),
                                               TCPChatRoom::connection_list{},
                                               chat_rooms.size() + 1);
    println("handle_create_and_sub: created room id={} name={}", room.id,
            room.name);
    return room;
  }();

  co_return std::pair{sub_to_room(chat_room, exec), &chat_room};
}

std::optional<TCPChatRoom *> Server::try_find_room(uint64_t room_id) {
  println("try_find_room: searching for room id={}", room_id);
  std::lock_guard<std::mutex> mut{chat_room_mutex};
  if (auto chat_room = std::ranges::find_if(
          chat_rooms, [&](const auto &room) { return room.id == room_id; });
      chat_room != chat_rooms.end()) {
    println("try_find_room: found room id={} name={}", chat_room->id,
            chat_room->name);
    return std::addressof(*chat_room);
  }
  println("try_find_room: room id={} not found", room_id);
  return std::nullopt;
}

boost::asio::awaitable<void>
Server::spawn_listener(tcp_socket &socket,
                       std::weak_ptr<Channel<std::string>> chan,
                       TCPChatRoom *chat_room) {
  println("spawn_listener: started for room id={} name={}", chat_room->id,
          chat_room->name);
  while (not chan.expired()) {
    auto locked = chan.lock();
    if (!locked || !locked->is_open() || !socket.is_open()) {
      break;
    }
    try {
      auto data = co_await locked->async_read();
      println("spawn_listener: room id={} sending message ({} bytes)",
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
        println("spawn_listener: client left room id={} name={}", chat_room->id,
                chat_room->name);
      } else {
        println("spawn_listener: error for room id={} name={}: {}",
                chat_room->id, chat_room->name, err.code().message());
      }
      break;
    } catch (const std::exception &err) {
      println("spawn_listener: unexpected error for room id={} name={}: {}",
              chat_room->id, chat_room->name, err.what());
      break;
    }
  }
  println("spawn_listener: exiting for room id={} name={}", chat_room->id,
          chat_room->name);
}

auto Server::parse_message(boost::json::object &data)
    -> std::variant<Message, std::string_view> {
  println("parse_message: parsing incoming message");
  if (not data.contains("grp")) {
    println("parse_message: missing grp field");
    return "no grp found";
  }
  if (not data.contains("msg")) {
    println("parse_message: missing msg field");
    return "no msg found";
  }

  if (auto grp_res = parse_id(data, "grp")) {
    auto grp = *grp_res;
    if (auto room = try_find_room(grp)) {
      auto message = std::string_view{data.at("msg").as_string()};
      println("parse_message: parsed grp id={} msg bytes={}", grp,
              message.size());
      return std::pair{message, *room};
    } else {
      println("parse_message: grp id={} not found", grp);
      return "grp not found";
    }
  } else {
    println("parse_message: invalid grp id");
    return "invalid grp";
  }
}

auto Server::parse_id(boost::json::object &data, std::string_view key)
    -> std::optional<uint64_t> {
  println("parse_id: parsing key '{}'", key);
  if (auto grp_result_uint64_t = data.at(key).try_as_uint64()) {
    println("parse_id: parsed {} as uint64 {}", key, *grp_result_uint64_t);
    return *grp_result_uint64_t;
  }
  if (auto grp_result_int64_t = data.at(key).try_as_int64()) {
    println("parse_id: parsed {} as int64 {}", key, *grp_result_int64_t);
    return static_cast<uint64_t>(*grp_result_int64_t);
  }

  println("parse_id: failed to parse key '{}'", key);
  return std::optional<uint64_t>{};
}
