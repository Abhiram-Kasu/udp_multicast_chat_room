#include "server.hpp"

Server::Server(int address, uint64_t max_room_limit)
    : m_listening_address(address), m_max_room_limit(max_room_limit),
      m_tcp_server_acceptor(boost::asio::ip::tcp::acceptor{
          m_io_context, boost::asio::ip::tcp::endpoint(
                            boost::asio::ip::tcp::v4(), m_listening_address)}) {

}

Server::~Server() = default;

void Server::serve() {
  boost::asio::co_spawn(m_io_context, accept_connections(),
                        boost::asio::detached);
  m_io_context.run();
}

boost::asio::awaitable<void> Server::accept_connections() {
  for (;;) {
    auto socket =
        co_await m_tcp_server_acceptor.async_accept(boost::asio::use_awaitable);
    co_await tcp_accept(std::move(socket));
  }
}

boost::asio::awaitable<std::vector<char>> Server::parse_data(tcp::socket &s) {

  auto mutable_buffer = boost::asio::streambuf();

  auto bytes_read = co_await boost::asio::async_read_until(
      s, mutable_buffer, ' ', boost::asio::use_awaitable);

  std::istream response_stream(&mutable_buffer);
  uint64_t data_size;
  response_stream >> data_size;

  std::vector<char> data(data_size);
  co_await s.async_read_some(boost::asio::buffer(data));
  co_return data;
}

boost::asio::awaitable<void>
Server::tcp_accept(boost::asio::ip::tcp::socket s) {
  // parse the first n bytes until the first space to see how much data is
  // incoming
  for (;;) {
    const auto data = co_await parse_data(s);
    std::string_view data_str{data.begin(), data.end()};
    boost::system::error_code error_code;
    auto structured_data = boost::json::parse(data_str, error_code).as_object();

    if (structured_data.contains("type")) {
      if (auto type = structured_data.at("type").try_as_string()) {
        // if the type
        auto &value = type.value();
        if (value == "sub") {
          // get code of the subbed group

        } else if (value == "desub") {

          co_return;

        } else [[likely]] if (value == "message") {
        }
      }
    }
  }
}
