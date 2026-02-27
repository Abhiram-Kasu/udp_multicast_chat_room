#include "server.hpp"
#include <boost/cobalt/main.hpp>

boost::cobalt::main co_main(int argc, char **argv) {
  auto server = Server{4040, 10};

  co_await server.serve();
  co_return 0;
}
