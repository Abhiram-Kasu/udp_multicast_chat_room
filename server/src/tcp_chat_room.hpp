#pragma once

#include "channel.hpp"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct TCPChatRoom {
  using connection_list = std::vector<std::weak_ptr<Channel<std::string>>>;
  std::mutex mutex{};
  std::string name;
  connection_list connections;
  uint64_t id;
  TCPChatRoom(const std::string &name, connection_list connections, uint64_t id)
      : name(name), connections(connections), id(id) {}
};
