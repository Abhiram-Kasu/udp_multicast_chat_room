#include "server.hpp"
#include <print>

auto main() -> int {

  auto server = Server{4040, 10};

  server.serve();
}
