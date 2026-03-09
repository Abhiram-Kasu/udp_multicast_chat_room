#pragma once

#include <boost/asio.hpp>
#include <deque>
#include <mutex>

template <typename Message> class Channel {
public:
  explicit Channel(boost::asio::any_io_executor exec)
      : executor_(exec), timer_(exec), closed_(false) {
    timer_.expires_at(boost::asio::steady_timer::time_point::max());
  }

  ~Channel() { close(); }

  Channel(const Channel &) = delete;
  Channel &operator=(const Channel &) = delete;

  bool is_open() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !closed_;
  }

  void close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    timer_.cancel();
  }

  void write(Message msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_)
      return;
    queue_.push_back(std::move(msg));
    timer_.cancel();
  }

  boost::asio::awaitable<Message> async_read() {
    for (;;) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!queue_.empty()) {
          Message msg = std::move(queue_.front());
          queue_.pop_front();
          co_return msg;
        }
        if (closed_) {
          throw boost::system::system_error(
              boost::asio::error::operation_aborted);
        }
        timer_.expires_at(boost::asio::steady_timer::time_point::max());
      }
      boost::system::error_code ec;
      co_await timer_.async_wait(
          boost::asio::redirect_error(boost::asio::use_awaitable, ec));
    }
  }

private:
  mutable std::mutex mutex_;
  std::deque<Message> queue_;
  boost::asio::any_io_executor executor_;
  boost::asio::steady_timer timer_;
  bool closed_;
};
