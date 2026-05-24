#pragma once

#include <boost/asio.hpp>
#include <map>
#include <queue>

#include "endpoint.hpp"

namespace gh {

class Icmp : public std::enable_shared_from_this<Icmp> {
public:
  class IcmpChannel;

  Icmp(boost::asio::io_context& io_context);
  Icmp(boost::asio::io_context& io_context, boost::asio::ip::icmp::endpoint bind);

  std::shared_ptr<Endpoint> CreateChannel(boost::asio::ip::icmp::endpoint const& peer);

private:
  using ReadHandlerMap = std::map<boost::asio::ip::icmp::endpoint, std::move_only_function<ReadHandler>>;

  std::weak_ptr<ReadHandlerMap> _ReadingChannel;
  boost::asio::ip::icmp::socket _Socket;
  boost::asio::ip::icmp::endpoint _Local;
  std::queue<std::tuple<boost::asio::ip::icmp::endpoint, const boost::asio::const_buffer,
                        std::move_only_function<WriteHandler>>>
      _WriteQueue;

  bool _ReadPending = false;
  bool _WritePending = false;

  enum State { kNone, kRunning, kError } _State = kNone;

  void TryAsyncStart(std::move_only_function<Event>&& handler);
  void AsyncStart(std::move_only_function<Event>&& handler);
  void Read(const boost::asio::ip::icmp::endpoint& peer, std::move_only_function<ReadHandler>&& handler);
  void Write(const boost::asio::ip::icmp::endpoint& peer, const boost::asio::const_buffer& b,
             std::move_only_function<WriteHandler>&& handler);
  void ScheduleRead(std::shared_ptr<ReadHandlerMap> m);
  void ScheduleWrite();
};

} // namespace gh
