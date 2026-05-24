#pragma once

#include <map>
#include <queue>

#include <boost/asio.hpp>

#include "endpoint.hpp"
#include "scoped_flag.hpp"

namespace gh {

class UdpMuxServer : public std::enable_shared_from_this<UdpMuxServer> {
public:
  class Channel;

  UdpMuxServer(boost::asio::io_context& io_context);
  UdpMuxServer(boost::asio::io_context& io_context, boost::asio::ip::udp::endpoint bind);

  std::shared_ptr<Endpoint> CreateChannel(uint8_t id);

  boost::asio::ip::udp::endpoint LocalEndpoint() const { return _Socket.local_endpoint(); }

private:
  using ReadHandlerMap = std::map<uint8_t, std::move_only_function<ReadHandler>>;
  // ReadHandler is holding the whole world, where we should break ref chain here
  std::weak_ptr<ReadHandlerMap> _ReadingChannel;
  std::map<uint8_t, boost::asio::ip::udp::endpoint> _Peers;
  boost::asio::ip::udp::socket _Socket;
  boost::asio::ip::udp::endpoint _Local;
  std::queue<std::tuple<uint8_t, Packet, std::move_only_function<WriteHandler>>> _WriteQueue;

  bool _ReadPending = false;
  bool _WritePending = false;

  enum State { kNone, kRunning, kError } _State = kNone;

  void TryAsyncStart(std::move_only_function<Event>&&);
  void AsyncStart(std::move_only_function<Event>&&);
  void Read(uint8_t id, std::move_only_function<ReadHandler>&& handler);
  void Write(uint8_t id, Packet&& b, std::move_only_function<WriteHandler>&& handler);
  void ScheduleRead(std::shared_ptr<ReadHandlerMap> m);
  void ScheduleWrite(ScopedFlag&& write);

  bool _Started = false;
};

} // namespace gh
