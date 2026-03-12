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
#include <expected>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

Server::Server(int address, uint64_t max_room_limit)
    : m_listening_port(address), m_max_room_limit(max_room_limit),
      m_tcp_server_acceptor(std::nullopt) {}

Server::~Server() = default;

void Server::serve(size_t thread_count) {
  log::println("Server::serve starting on port {} with {} threads",
               m_listening_port, thread_count);

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
            log::println("server error: {}", ex.what());
          }
        }
      });

  std::vector<std::jthread> threads;
  threads.reserve(thread_count - 1);
  for (size_t i = 1; i < thread_count; ++i) {
    threads.emplace_back([&io_context, i]() {
      log::println("worker thread {} started", i);
      io_context.run();
      log::println("worker thread {} finished", i);
    });
  }

  log::println("main thread entering io_context::run()");
  io_context.run();
  log::println("main thread exiting");
}

boost::asio::awaitable<void> Server::serve_async() {
  log::println("Server::serve_async starting on port {}", m_listening_port);
  auto exec = co_await boost::asio::this_coro::executor;

  m_tcp_server_acceptor.emplace(exec);
  auto &acceptor = *m_tcp_server_acceptor;
  acceptor.open(tcp::v4());
  acceptor.set_option(boost::asio::socket_base::reuse_address(true));
  acceptor.bind(tcp::endpoint(tcp::v4(), m_listening_port));
  acceptor.listen();

  log::println("Server::serve_async acceptor ready");
  log::println("listening on {}", m_listening_port);

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
            log::println("accept_connections error: {}", ex.what());
          }
        }
      });

  log::println("Server::serve_async waiting for shutdown signal");
  boost::system::error_code ec;
  auto sig = co_await signals.async_wait(
      boost::asio::redirect_error(boost::asio::use_awaitable, ec));
  if (!ec) {
    log::println("Server::serve_async shutdown signal received ({})", sig);
  }

  if (m_tcp_server_acceptor) {
    boost::system::error_code close_ec;
    m_tcp_server_acceptor->close(close_ec);
    if (close_ec) {
      log::println("Server::serve_async acceptor close error: {}",
                   close_ec.message());
    } else {
      log::println("Server::serve_async acceptor closed");
    }
  }

  log::println("Server::serve_async accept loop exited");
}

boost::asio::awaitable<void> Server::accept_connections() {
  if (not m_tcp_server_acceptor) {
    log::println("accept_connections: no acceptor, returning");
    co_return;
  }

  auto exec = co_await boost::asio::this_coro::executor;
  auto &acceptor = *m_tcp_server_acceptor;

  for (;;) {
    log::println("accept_connections: waiting for connection");
    try {
      auto socket = co_await acceptor.async_accept();
      log::println("accept_connections: received connection");

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
                log::println("tcp_accept session error: {}", ex.what());
              }
            }
          });
    } catch (const boost::system::system_error &err) {
      log::println("accept_connections: accept error: {}", err.what());
      co_return;
    }
  }
}

boost::asio::awaitable<std::vector<char>>
Server::parse_data(tcp_socket &s, boost::asio::streambuf &buf) {
  log::println("parse_data: waiting for newline-delimited payload");

  auto bytes_read = co_await boost::asio::async_read_until(
      s, buf, '\n', boost::asio::use_awaitable);
  log::println("parse_data: read {} bytes, streambuf size = {}", bytes_read,
               buf.size());

  std::istream response_stream(&buf);
  std::string data_str(bytes_read, '\0');
  response_stream.read(data_str.data(), bytes_read);

  log::print("parse_data: raw hex dump ({} bytes): ", data_str.size());
  for (unsigned char c : data_str) {
    log::print("{:02x} ", c);
  }
  log::println("");
  log::println("parse_data: raw string repr: [{}]", data_str);

  while (!data_str.empty() &&
         (data_str.back() == '\n' || data_str.back() == '\r')) {
    log::println("parse_data: trimming trailing byte 0x{:02x}",
                 static_cast<unsigned char>(data_str.back()));
    data_str.pop_back();
  }

  log::print("parse_data: trimmed hex dump ({} bytes): ", data_str.size());
  for (unsigned char c : data_str) {
    log::print("{:02x} ", c);
  }
  log::println("");
  log::println("parse_data: trimmed string repr: [{}]", data_str);

  std::vector<char> data(data_str.begin(), data_str.end());
  log::println("parse_data: returning {} bytes", data.size());
  co_return data;
}

boost::asio::awaitable<void> Server::tcp_accept(tcp_socket s) {
  log::println("tcp_accept: session started");
  auto exec = co_await boost::asio::this_coro::executor;
  auto current_subscriptions = std::map<TCPChatRoom *, subscription>{};
  auto read_buf = boost::asio::streambuf{};

  auto write_chan = std::make_shared<Channel<std::string>>(exec);

  boost::asio::co_spawn(
      exec,
      [&s, wc = write_chan]() -> boost::asio::awaitable<void> {
        co_await Server::socket_writer(s, wc);
      },
      boost::asio::detached);

  auto cleanup = [&]() {
    for (auto &[room, sub] : current_subscriptions) {
      sub.first->close();
    }
    write_chan->close();
  };

  for (;;) {
    try {
      log::println("tcp_accept: waiting for next message");
      const auto data = co_await parse_data(s, read_buf);
      log::println("tcp_accept: received raw data ({} bytes): {}", data.size(),
                   std::string_view{data.begin(), data.end()});

      if (data.empty()) {
        log::println("tcp_accept: received empty line, ignoring");
        continue;
      }

      std::string_view data_str{data.begin(), data.end()};

      log::print("tcp_accept: about to parse JSON hex ({} bytes): ",
                 data_str.size());
      for (unsigned char c : data_str) {
        log::print("{:02x} ", c);
      }
      log::println("");
      log::println("tcp_accept: about to parse JSON string: [{}]", data_str);

      boost::system::error_code error_code;
      auto parsed = boost::json::parse(data_str, error_code);
      if (error_code.failed()) {
        log::println("tcp_accept: failed to parse JSON: {} (error code: {})",
                     error_code.message(), error_code.value());
        write_chan->write("Failed to parse JSON: " + error_code.message() +
                          "\n");
        continue;
      }
      if (!parsed.is_object()) {
        log::println("tcp_accept: invalid JSON (not an object)");
        write_chan->write("Invalid JSON\n");
        continue;
      }

      auto structured_data = parsed.as_object();

      if (structured_data.contains("type")) {
        auto type_val = structured_data.at("type");
        if (type_val.is_string()) {
          const std::string value{type_val.as_string().c_str()};
          log::println("tcp_accept: message type={}", value);

          if (value == "sub") {
            log::println("tcp_accept: handling sub request");

            auto result =
                co_await handle_sub(structured_data, current_subscriptions);
            if (result.has_value()) {
              auto &[channel, chat_room] = *result;

              auto weak_chan = std::weak_ptr<Channel<std::string>>{channel};
              auto *cr = chat_room;
              boost::asio::co_spawn(
                  exec,
                  [wc = write_chan, weak_chan,
                   cr]() -> boost::asio::awaitable<void> {
                    co_await Server::spawn_listener(wc, weak_chan, cr);
                  },
                  boost::asio::detached);

              auto chan_copy = channel;
              current_subscriptions.emplace(
                  chat_room, subscription{chan_copy, std::move(chan_copy)});
              log::println("tcp_accept: subscribed to room id={} name={}",
                           chat_room->id, chat_room->name);

              auto reply = boost::json::serialize(
                  boost::json::object{{"type", "sub_ok"},
                                      {"grp", chat_room->id},
                                      {"grp_name", chat_room->name}});
              reply += '\n';
              write_chan->write(std::move(reply));
            } else {
              auto error = result.error();
              log::println("tcp_accept: sub failed: {}", error);
              write_chan->write(std::format("Failed: {}\n", error));
            }
          } else if (value == "desub") {
            log::println("tcp_accept: desub request received, ending session");
            cleanup();
            co_return;
          } else if (value == "csub") {
            log::println("tcp_accept: handling create+sub request");
            auto result = co_await handle_create_and_sub(structured_data);
            if (result.has_value()) {
              auto &[channel, chat_room] = *result;

              auto weak_chan = std::weak_ptr<Channel<std::string>>{channel};
              auto *cr = chat_room;
              boost::asio::co_spawn(
                  exec,
                  [wc = write_chan, weak_chan,
                   cr]() -> boost::asio::awaitable<void> {
                    co_await Server::spawn_listener(wc, weak_chan, cr);
                  },
                  boost::asio::detached);

              auto chan_copy = channel;
              current_subscriptions.emplace(
                  chat_room, subscription{chan_copy, std::move(chan_copy)});
              log::println("tcp_accept: created+subscribed room id={} name={}",
                           chat_room->id, chat_room->name);

              auto reply = boost::json::serialize(
                  boost::json::object{{"type", "csub_ok"},
                                      {"grp", chat_room->id},
                                      {"grp_name", chat_room->name}});
              reply += '\n';
              write_chan->write(std::move(reply));
            } else {
              auto error = result.error();
              log::println("tcp_accept: create+sub failed: {}", error);
              write_chan->write(
                  std::format("Failed to create and subscribe: {}\n", error));
            }

          } else if (value == "msg") {
            log::println("tcp_accept: handling message request");

            auto msg_result = parse_message(structured_data);
            if (msg_result.has_value()) {
              auto &&[message, chat_room] = *msg_result;
              std::lock_guard guard{chat_room->mutex};

              if (not current_subscriptions.contains(chat_room)) {
                log::println("tcp_accept: message rejected (not subscribed)");
                write_chan->write("not subscribed to this room\n");
                continue;
              }

              auto &curr_chan = current_subscriptions.at(chat_room).second;

              auto recipient_count = std::ranges::count_if(
                  chat_room->connections,
                  [&](std::weak_ptr<Channel<std::string>> chan) {
                    return chan.lock() != curr_chan;
                  });
              log::println("tcp_accept: broadcasting message ({} bytes) to {} "
                           "recipients in room id={} name={}",
                           message.size(), recipient_count, chat_room->id,
                           chat_room->name);

              for (const auto &connection : chat_room->connections) {
                auto locked = connection.lock();
                if (locked && locked != curr_chan) {
                  locked->write(std::string{message});
                }
              }

              log::println("tcp_accept: broadcast complete");
            } else {
              auto error = msg_result.error();
              log::println("tcp_accept: message parse error: {}", error);
              write_chan->write(std::format("Failed to send msg: {}\n", error));
            }

          } else {
            log::println("tcp_accept: Invalid JSON (unknown type)");
            write_chan->write("Invalid JSON\n");
          }
        } else {
          log::println("tcp_accept: type field is not a string");
          write_chan->write("Invalid JSON\n");
        }
      } else {
        log::println("tcp_accept: missing type field");
        write_chan->write("Invalid JSON\n");
      }
    } catch (const boost::system::system_error &err) {
      if (err.code() == boost::asio::error::eof ||
          err.code() == boost::asio::error::connection_reset ||
          err.code() == boost::asio::error::broken_pipe) {
        log::println("tcp_accept: client disconnected");
      } else {
        log::println("tcp_accept: error: {}", err.code().message());
      }
      cleanup();
      co_return;
    } catch (const std::exception &err) {
      log::println("tcp_accept: unexpected error: {}", err.what());
      cleanup();
      co_return;
    }
  }
}

boost::asio::awaitable<std::expected<
    std::pair<std::shared_ptr<Channel<std::string>>, TCPChatRoom *>,
    std::string_view>>
Server::handle_sub(boost::json::object &structured_data,
                   const std::map<TCPChatRoom *, subscription> &listener_map) {
  log::println("handle_sub: processing subscription request");
  auto exec = co_await boost::asio::this_coro::executor;

  if (not structured_data.contains("grp")) {
    log::println("handle_sub: missing grp field");
    co_return std::unexpected{"grp not found"};
  }

  const auto grp_res = parse_id(structured_data, "grp");

  if (not grp_res) {
    log::println("handle_sub: invalid grp id");
    co_return std::unexpected{"grp not valid"};
  }
  auto grp = *grp_res;
  log::println("handle_sub: parsed grp id={}", grp);

  if (std::ranges::any_of(listener_map, [=](const auto &kv) {
        const auto &[k, _] = kv;
        return k->id == grp;
      })) {
    log::println("handle_sub: already subscribed to grp id={}", grp);
    co_return std::unexpected{"Already subscribed to this Chat Room"};
  }

  if (auto chat_room_res = try_find_room(grp)) {
    auto *chat_room = *chat_room_res;
    log::println("handle_sub: found room id={} name={}", chat_room->id,
                 chat_room->name);
    co_return std::pair{sub_to_room(*chat_room, exec), chat_room};
  } else {
    log::println("handle_sub: no such group id={}", grp);
    co_return std::unexpected{"No such group exists"};
  }
}

std::shared_ptr<Channel<std::string>>
Server::sub_to_room(TCPChatRoom &chat_room, boost::asio::any_io_executor exec) {
  log::println("sub_to_room: subscribing to room id={} name={}", chat_room.id,
               chat_room.name);
  auto channel = std::make_shared<Channel<std::string>>(exec);
  std::lock_guard<std::mutex> guard{chat_room.mutex};
  chat_room.connections.push_back(channel);
  log::println("sub_to_room: room id={} now has {} connections", chat_room.id,
               chat_room.connections.size());
  return channel;
}

boost::asio::awaitable<std::expected<
    std::pair<std::shared_ptr<Channel<std::string>>, TCPChatRoom *>,
    std::string_view>>
Server::handle_create_and_sub(boost::json::object &data) {
  log::println("handle_create_and_sub: processing create+sub request");
  auto exec = co_await boost::asio::this_coro::executor;

  if (not data.contains("grp_name")) {
    log::println("handle_create_and_sub: missing grp_name");
    co_return std::unexpected{"'grp_name' not found"};
  }
  if (not data.at("grp_name").is_string()) {
    log::println("handle_create_and_sub: grp_name invalid format");
    co_return std::unexpected{"'grp_name' in invalid format"};
  }

  auto &chat_room = [&]() -> decltype(auto) {
    const auto grp_name = std::string_view{data.at("grp_name").as_string()};

    std::lock_guard<std::mutex> guard{this->chat_room_mutex};
    auto &room = this->chat_rooms.emplace_back(std::string(grp_name),
                                               TCPChatRoom::connection_list{},
                                               chat_rooms.size() + 1);
    log::println("handle_create_and_sub: created room id={} name={}", room.id,
                 room.name);
    return room;
  }();

  co_return std::pair{sub_to_room(chat_room, exec), &chat_room};
}

std::optional<TCPChatRoom *> Server::try_find_room(uint64_t room_id) {
  log::println("try_find_room: searching for room id={}", room_id);
  std::lock_guard<std::mutex> mut{chat_room_mutex};
  if (auto chat_room = std::ranges::find_if(
          chat_rooms, [&](const auto &room) { return room.id == room_id; });
      chat_room != chat_rooms.end()) {
    log::println("try_find_room: found room id={} name={}", chat_room->id,
                 chat_room->name);
    return std::addressof(*chat_room);
  }
  log::println("try_find_room: room id={} not found", room_id);
  return std::nullopt;
}

boost::asio::awaitable<void>
Server::socket_writer(tcp_socket &s,
                      std::shared_ptr<Channel<std::string>> write_chan) {
  log::println("socket_writer: started");
  while (write_chan->is_open() || true) {
    try {
      auto data = co_await write_chan->async_read();
      log::println("socket_writer: sending {} bytes", data.size());
      co_await s.async_send(boost::asio::buffer(data));
    } catch (const boost::system::system_error &err) {
      if (err.code() == boost::asio::error::operation_aborted) {
        log::println("socket_writer: channel closed, exiting");
      } else {
        log::println("socket_writer: error: {}", err.code().message());
      }
      break;
    } catch (const std::exception &err) {
      log::println("socket_writer: unexpected error: {}", err.what());
      break;
    }
  }
  log::println("socket_writer: exiting");
}

boost::asio::awaitable<void>
Server::spawn_listener(std::shared_ptr<Channel<std::string>> write_chan,
                       std::weak_ptr<Channel<std::string>> read_chan,
                       TCPChatRoom *chat_room) {
  log::println("spawn_listener: started for room id={} name={}", chat_room->id,
               chat_room->name);
  while (not read_chan.expired()) {
    auto locked = read_chan.lock();
    if (!locked || !locked->is_open()) {
      break;
    }
    try {
      auto data = co_await locked->async_read();
      log::println("spawn_listener: room id={} forwarding message ({} bytes)",
                   chat_room->id, data.size());
      auto json_str = boost::json::serialize(boost::json::object{
          {"type", "message"}, {"data", data}, {"grp_name", chat_room->name}});
      json_str += '\n';
      write_chan->write(std::move(json_str));
    } catch (const boost::system::system_error &err) {
      if (err.code() == boost::asio::error::eof ||
          err.code() == boost::asio::error::connection_reset ||
          err.code() == boost::asio::error::broken_pipe ||
          err.code() == boost::asio::error::operation_aborted ||
          err.code().value() == 89 /* ECANCELED */) {
        log::println("spawn_listener: client left room id={} name={}",
                     chat_room->id, chat_room->name);
      } else {
        log::println("spawn_listener: error for room id={} name={}: {}",
                     chat_room->id, chat_room->name, err.code().message());
      }
      break;
    } catch (const std::exception &err) {
      log::println(
          "spawn_listener: unexpected error for room id={} name={}: {}",
          chat_room->id, chat_room->name, err.what());
      break;
    }
  }
  log::println("spawn_listener: exiting for room id={} name={}", chat_room->id,
               chat_room->name);
}

auto Server::parse_message(boost::json::object &data)
    -> std::expected<Message, std::string_view> {
  log::println("parse_message: parsing incoming message");
  if (not data.contains("grp")) {
    log::println("parse_message: missing grp field");
    return std::unexpected{"no grp found"};
  }
  if (not data.contains("msg")) {
    log::println("parse_message: missing msg field");
    return std::unexpected{"no msg found"};
  }

  if (auto grp_res = parse_id(data, "grp")) {
    auto grp = *grp_res;
    if (auto room = try_find_room(grp)) {
      auto message = std::string_view{data.at("msg").as_string()};
      log::println("parse_message: parsed grp id={} msg bytes={}", grp,
                   message.size());
      return std::pair{message, *room};
    } else {
      log::println("parse_message: grp id={} not found", grp);
      return std::unexpected{"grp not found"};
    }
  } else {
    log::println("parse_message: invalid grp id");
    return std::unexpected{"invalid grp"};
  }
}

auto Server::parse_id(boost::json::object &data, std::string_view key)
    -> std::optional<uint64_t> {
  log::println("parse_id: parsing key '{}'", key);
  if (auto grp_result_uint64_t = data.at(key).try_as_uint64()) {
    log::println("parse_id: parsed {} as uint64 {}", key, *grp_result_uint64_t);
    return *grp_result_uint64_t;
  }
  if (auto grp_result_int64_t = data.at(key).try_as_int64()) {
    log::println("parse_id: parsed {} as int64 {}", key, *grp_result_int64_t);
    return static_cast<uint64_t>(*grp_result_int64_t);
  }

  log::println("parse_id: failed to parse key '{}'", key);
  return std::optional<uint64_t>{};
}
