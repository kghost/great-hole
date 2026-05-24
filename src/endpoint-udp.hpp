#pragma once

#include <map>
#include <queue>

#include <boost/asio.hpp>

#include "endpoint.hpp"

namespace gh {

class Udp : public std::enable_shared_from_this<Udp> {
public:
  class UdpChannel;

  Udp(boost::asio::io_context& io_context);
  Udp(boost::asio::io_context& io_context, boost::asio::ip::udp::endpoint bind);

  std::shared_ptr<Endpoint> CreateChannel(boost::asio::ip::udp::endpoint const& peer);

private:
  using ReadHandlerMap = std::map<boost::asio::ip::udp::endpoint, std::move_only_function<ReadHandler>>;
  // ReadHandler is holding the whole world, where we should break ref chain here
  std::weak_ptr<ReadHandlerMap> _ReadingChannel;
  boost::asio::ip::udp::socket _Socket;
  boost::asio::ip::udp::endpoint _Local;
  std::queue<std::tuple<boost::asio::ip::udp::endpoint, Packet, std::move_only_function<WriteHandler>>> _WriteQueue;

  bool _ReadPending = false;
  bool _WritePending = false;

  enum State { kNone, kRunning, kError } _State = kNone;

  void TryAsyncStart(std::move_only_function<Event>&&);
  void AsyncStart(std::move_only_function<Event>&&);
  void Read(boost::asio::ip::udp::endpoint const& peer, std::move_only_function<ReadHandler>&& handler);
  void Write(boost::asio::ip::udp::endpoint const& peer, Packet&& p, std::move_only_function<WriteHandler>&& handler);
  void ScheduleRead(std::shared_ptr<ReadHandlerMap> m);
  void ScheduleWrite();

  bool _Started = false;
};

} // namespace gh
