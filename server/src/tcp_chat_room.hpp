
#pragma once
#include "boost/cobalt.hpp"
#include <memory>
#include <mutex>
#include <vector>

template <typename Message> using Channel = boost::cobalt::channel<Message>;

struct TCPChatRoom {
  using connection_list = std::vector<std::weak_ptr<Channel<std::string>>>;
  std::mutex mutex{};
  std::string name;
  connection_list connections;
  uint64_t id;
  TCPChatRoom(const std::string &name, connection_list connections, uint64_t id)
      : name(name), connections(connections), id(id) {}
};
