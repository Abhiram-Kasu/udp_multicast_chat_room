#include "server.hpp"
#include <string>

int main(int argc, char **argv) {

  auto tc = std::optional<size_t>{};
  if (argc == 2) {
    auto tc_param = std::string_view{argv[1]};
    if (tc_param.starts_with("-j")) {
      tc = std::stoull(std::string{tc_param.substr(2)});
    }
  }

  auto server = Server{4040, 10};
  if (tc) {
    server.serve(*tc);
  } else {
    server.serve();
  }
  return 0;
}
