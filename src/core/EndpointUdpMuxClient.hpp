#pragma once

#include <boost/asio.hpp>

#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "Endpoint.hpp"
#include "ErrorCode.hpp"

namespace gh {

class UdpMuxClient : public Endpoint {
public:
  explicit UdpMuxClient(boost::asio::io_context& ioContext, uint8_t id, boost::asio::ip::udp::endpoint peer);
  explicit UdpMuxClient(boost::asio::io_context& ioContext, uint8_t id, boost::asio::ip::udp::endpoint peer,
                        boost::asio::ip::udp::endpoint local);
  ~UdpMuxClient() override;

  UdpMuxClient(const UdpMuxClient&) = delete;
  UdpMuxClient& operator=(const UdpMuxClient&) = delete;
  UdpMuxClient(UdpMuxClient&&) = delete;
  UdpMuxClient& operator=(UdpMuxClient&&) = delete;

  Omni::Fiber::Coroutine<ErrorCode> Read(Packet& p, Cancel& c) override;
  Omni::Fiber::Coroutine<ErrorCode> Write(Packet& p, Cancel& c) override;

protected:
  std::string GetName() const override;
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

public:
  boost::asio::ip::udp::endpoint LocalEndpoint() const { return _Socket.local_endpoint(); }

private:
  boost::asio::ip::udp::socket _Socket;
  const uint8_t _Id;
  const boost::asio::ip::udp::endpoint _Local;
  const boost::asio::ip::udp::endpoint _Peer;
};

} // namespace gh
