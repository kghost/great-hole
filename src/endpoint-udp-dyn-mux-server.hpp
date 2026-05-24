#pragma once

#include <chrono>
#include <map>
#include <memory>

#include <boost/asio.hpp>

#include "endpoint.hpp"

namespace gh {

class UdpDynMuxServer : public std::enable_shared_from_this<UdpDynMuxServer> {
public:
  UdpDynMuxServer(boost::asio::io_context& io_context);
  UdpDynMuxServer(boost::asio::io_context& io_context, boost::asio::ip::udp::endpoint bind);

  boost::asio::ip::udp::endpoint LocalEndpoint() const { return _Socket.local_endpoint(); }

  void AsyncStart(std::move_only_function<Event>&& handler);

private:
  struct ClientSession {
    boost::asio::ip::udp::endpoint Peer;
    uint32_t Cookie = 0;
    std::chrono::steady_clock::time_point LastSeen;
    int MissingAcks = 0;
  };
  std::map<uint16_t, ClientSession> _Clients;

  boost::asio::ip::udp::socket _Socket;
  boost::asio::ip::udp::endpoint _Local;

  bool _ReadPending = false;

  enum State { kNone, kRunning, kError } _State = kNone;
  bool _Started = false;

  // Timers
  boost::asio::steady_timer _KeepaliveTimer;

  // Rate limiting timestamps based on peer address
  std::map<boost::asio::ip::udp::endpoint, std::chrono::steady_clock::time_point> _LastErrorSent;

  bool CheckRateLimit(const boost::asio::ip::udp::endpoint& peer);

  void ScheduleRead();

  void StartKeepaliveTimer();
  void HandleKeepaliveTick();

  void SendControlAssign(const boost::asio::ip::udp::endpoint& peer, uint32_t cookie, uint16_t id);
  void SendControlKeepalive(const boost::asio::ip::udp::endpoint& peer, uint16_t id);
  void SendControlIdClosed(const boost::asio::ip::udp::endpoint& peer, uint16_t id);
  void SendControlAddrMismatch(const boost::asio::ip::udp::endpoint& peer, uint16_t id);
  void SendControlMigrateAck(const boost::asio::ip::udp::endpoint& peer, uint16_t id);
  void SendControlInvalidId(const boost::asio::ip::udp::endpoint& peer, uint16_t id);
  void SendControlCookieMismatch(const boost::asio::ip::udp::endpoint& peer, uint16_t id);
};

} // namespace gh
