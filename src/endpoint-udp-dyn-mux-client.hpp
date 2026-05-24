#pragma once

#include <boost/asio.hpp>
#include <chrono>
#include <memory>
#include <queue>
#include <random>

#include "endpoint.hpp"

class udp_dyn_mux_client : public std::enable_shared_from_this<udp_dyn_mux_client>, public endpoint {
public:
  udp_dyn_mux_client(boost::asio::io_context& io_context, boost::asio::ip::udp::endpoint peer);
  udp_dyn_mux_client(boost::asio::io_context& io_context, boost::asio::ip::udp::endpoint peer,
                     boost::asio::ip::udp::endpoint local);

  void async_start(std::move_only_function<event>&&) override;
  void async_read(std::move_only_function<read_handler>&&) override;
  void async_write(packet&&, std::move_only_function<write_handler>&&) override;

  boost::asio::ip::udp::endpoint local_endpoint() const { return socket.local_endpoint(); }

private:
  boost::asio::ip::udp::socket socket;
  const boost::asio::ip::udp::endpoint local;
  const boost::asio::ip::udp::endpoint peer;

  bool write_pending = false;

  enum state_t { none, negotiating, running, migrating, error } state = none;

  uint16_t assigned_id = 0;
  uint32_t current_cookie = 0;

  // Packet queues
  std::queue<std::pair<packet, std::move_only_function<write_handler>>> write_queue;
  std::queue<packet> incoming_data_queue;

  // Timers
  boost::asio::steady_timer negotiate_timer;
  boost::asio::steady_timer migrate_timer;
  boost::asio::steady_timer keepalive_check_timer;
  std::chrono::steady_clock::time_point last_keepalive_received;

  // Handler callbacks
  std::move_only_function<event> start_handler;
  std::move_only_function<read_handler> read_handler_cb;

  // PRNG
  std::mt19937 prng;

  void start_negotiation();
  void send_negotiate_request();
  void start_migration();
  void send_migration_request();
  void check_keepalive_timeout();
  void schedule_socket_read();
  void handle_control_packet(const uint8_t* data, std::size_t length);
  void flush_write_queue();
};
