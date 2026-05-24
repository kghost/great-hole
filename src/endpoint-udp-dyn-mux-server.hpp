#pragma once

#include <chrono>
#include <map>
#include <memory>

#include <boost/asio.hpp>

#include "endpoint.hpp"

class udp_dyn_mux_server : public std::enable_shared_from_this<udp_dyn_mux_server> {
public:
  udp_dyn_mux_server(boost::asio::io_context& io_context);
  udp_dyn_mux_server(boost::asio::io_context& io_context, boost::asio::ip::udp::endpoint bind);

  boost::asio::ip::udp::endpoint local_endpoint() const { return socket.local_endpoint(); }

  void async_start(std::move_only_function<event>&& handler);

private:
  struct client_session {
    boost::asio::ip::udp::endpoint peer;
    uint32_t cookie = 0;
    std::chrono::steady_clock::time_point last_seen;
    int missing_acks = 0;
  };
  std::map<uint16_t, client_session> clients;

  boost::asio::ip::udp::socket socket;
  boost::asio::ip::udp::endpoint local;

  bool read_pending = false;

  enum { none, running, error } state = none;
  bool started = false;

  // Timers
  boost::asio::steady_timer keepalive_timer;

  // Rate limiting timestamps based on peer address
  std::map<boost::asio::ip::udp::endpoint, std::chrono::steady_clock::time_point> last_error_sent;

  bool check_rate_limit(const boost::asio::ip::udp::endpoint& peer);

  void schedule_read();

  void start_keepalive_timer();
  void handle_keepalive_tick();

  void send_control_assign(const boost::asio::ip::udp::endpoint& peer, uint32_t cookie, uint16_t id);
  void send_control_keepalive(const boost::asio::ip::udp::endpoint& peer, uint16_t id);
  void send_control_id_closed(const boost::asio::ip::udp::endpoint& peer, uint16_t id);
  void send_control_addr_mismatch(const boost::asio::ip::udp::endpoint& peer, uint16_t id);
  void send_control_migrate_ack(const boost::asio::ip::udp::endpoint& peer, uint16_t id);
  void send_control_invalid_id(const boost::asio::ip::udp::endpoint& peer, uint16_t id);
  void send_control_cookie_mismatch(const boost::asio::ip::udp::endpoint& peer, uint16_t id);
};
