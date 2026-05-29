#pragma once

#include <chrono>
#include <expected>
#include <random>

#include <boost/asio.hpp>

#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "Endpoint.hpp"
#include "ErrorCode.hpp"
#include "Pipe.hpp"
#include "EndpointUdpDynMuxProtocol.hpp"

namespace gh {

class UdpDynMuxClient : public Endpoint {
public:
  explicit UdpDynMuxClient(boost::asio::io_context& ioContext, boost::asio::ip::udp::endpoint peer);
  explicit UdpDynMuxClient(boost::asio::io_context& ioContext, boost::asio::ip::udp::endpoint peer,
                           boost::asio::ip::udp::endpoint local);
  ~UdpDynMuxClient() override;

  UdpDynMuxClient(const UdpDynMuxClient&) = delete;
  UdpDynMuxClient& operator=(const UdpDynMuxClient&) = delete;
  UdpDynMuxClient(UdpDynMuxClient&&) = delete;
  UdpDynMuxClient& operator=(UdpDynMuxClient&&) = delete;

  Omni::Fiber::Coroutine<ErrorCode> Read(Packet& p, Cancel& c) override;
  Omni::Fiber::Coroutine<ErrorCode> Write(Packet& p, Cancel& c) override;

  boost::asio::ip::udp::endpoint LocalEndpoint() const { return _Socket.local_endpoint(); }

protected:
  std::string GetName() const override;
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<void> DoWork() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

private:
  struct ControlEvent {
    UdpDynMux::MsgType Type;
    uint32_t Cookie = 0;
    uint16_t Id = 0;
  };

  Omni::Fiber::Coroutine<void> ReadLoop();
  Omni::Fiber::Coroutine<void> ClientLoop();
  Omni::Fiber::Coroutine<void> HandleControlPacket(const uint8_t* data, std::size_t length);

  boost::asio::ip::udp::socket _Socket;
  const boost::asio::ip::udp::endpoint _Local;
  const boost::asio::ip::udp::endpoint _Peer;

  enum State { kNone, kNegotiating, kRunning, kMigrating, kError } _State = kNone;

  uint16_t _AssignedId = 0;
  uint32_t _CurrentCookie = 0;
  std::chrono::steady_clock::time_point _LastKeepaliveReceived;

  std::mt19937 _Prng;

  Omni::Fiber::Pipe<std::expected<Packet, ErrorCode>> _DataPipe;
  Omni::Fiber::Pipe<ControlEvent> _ControlPipe;
};

} // namespace gh
