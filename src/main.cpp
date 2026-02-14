#include "server.hpp"
#include <print>

#include <boost/asio.hpp>

auto main() -> int {

  auto server = Server{};

  server.serve();
}
