#pragma once

#include <boost/asio.hpp>

#include "endpoint.hpp"

namespace gh {

class UdpMuxClient : public std::enable_shared_from_this<UdpMuxClient>, public Endpoint {
public:
  UdpMuxClient(boost::asio::io_context& io_context, uint8_t id, boost::asio::ip::udp::endpoint peer);
  UdpMuxClient(boost::asio::io_context& io_context, uint8_t id, boost::asio::ip::udp::endpoint peer,
               boost::asio::ip::udp::endpoint local);

  void AsyncStart(std::move_only_function<Event>&&) override;
  void AsyncRead(std::move_only_function<ReadHandler>&&) override;
  void AsyncWrite(Packet&&, std::move_only_function<WriteHandler>&&) override;

  boost::asio::ip::udp::endpoint LocalEndpoint() const { return _Socket.local_endpoint(); }

private:
  boost::asio::ip::udp::socket _Socket;
  const uint8_t _Id;
  const boost::asio::ip::udp::endpoint _Local;
  const boost::asio::ip::udp::endpoint _Peer;

  bool _ReadPending = false;
  bool _WritePending = false;

  enum State { kNone, kOpening, kRunning, kError } _State = kNone;
};

} // namespace gh
