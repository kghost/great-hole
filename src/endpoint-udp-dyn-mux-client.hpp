#pragma once

#include <boost/asio.hpp>
#include <chrono>
#include <memory>
#include <queue>
#include <random>

#include "endpoint.hpp"

namespace gh {

class UdpDynMuxClient : public std::enable_shared_from_this<UdpDynMuxClient>, public Endpoint {
public:
  UdpDynMuxClient(boost::asio::io_context& io_context, boost::asio::ip::udp::endpoint peer);
  UdpDynMuxClient(boost::asio::io_context& io_context, boost::asio::ip::udp::endpoint peer,
                  boost::asio::ip::udp::endpoint local);

  void AsyncStart(std::move_only_function<Event>&&) override;
  void AsyncRead(std::move_only_function<ReadHandler>&&) override;
  void AsyncWrite(Packet&&, std::move_only_function<WriteHandler>&&) override;

  boost::asio::ip::udp::endpoint LocalEndpoint() const { return _Socket.local_endpoint(); }

private:
  boost::asio::ip::udp::socket _Socket;
  const boost::asio::ip::udp::endpoint _Local;
  const boost::asio::ip::udp::endpoint _Peer;

  bool _WritePending = false;

  enum State { kNone, kNegotiating, kRunning, kMigrating, kError } _State = kNone;

  uint16_t _AssignedId = 0;
  uint32_t _CurrentCookie = 0;

  // Packet queues
  std::queue<std::pair<Packet, std::move_only_function<WriteHandler>>> _WriteQueue;
  std::queue<Packet> _IncomingDataQueue;

  // Timers
  boost::asio::steady_timer _NegotiateTimer;
  boost::asio::steady_timer _MigrateTimer;
  boost::asio::steady_timer _KeepaliveCheckTimer;
  std::chrono::steady_clock::time_point _LastKeepaliveReceived;

  // Handler callbacks
  std::move_only_function<Event> _StartHandler;
  std::move_only_function<ReadHandler> _ReadHandlerCb;

  // PRNG
  std::mt19937 _Prng;

  void StartNegotiation();
  void SendNegotiateRequest();
  void StartMigration();
  void SendMigrationRequest();
  void CheckKeepaliveTimeout();
  void ScheduleSocketRead();
  void HandleControlPacket(const uint8_t* data, std::size_t length);
  void FlushWriteQueue();
};

} // namespace gh
