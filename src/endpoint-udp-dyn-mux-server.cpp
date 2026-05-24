#include "endpoint-udp-dyn-mux-server.hpp"

#include <algorithm>
#include <boost/asio/buffer.hpp>
#include <boost/log/trivial.hpp>
#include <cassert>

#include "endpoint-udp-dyn-mux-protocol.hpp"

udp_dyn_mux_server::udp_dyn_mux_server(boost::asio::io_context& io_context)
    : socket(io_context), local(boost::asio::ip::udp::v6(), 0), keepalive_timer(io_context) {}

udp_dyn_mux_server::udp_dyn_mux_server(boost::asio::io_context& io_context, boost::asio::ip::udp::endpoint bind)
    : socket(io_context), local(bind), keepalive_timer(io_context) {}

void udp_dyn_mux_server::async_start(std::move_only_function<event>&& handler) {
  switch (state) {
  case none:
    try {
      socket.open(boost::asio::ip::udp::v6());
      socket.set_option(boost::asio::ip::v6_only(false));
      socket.bind(local);
      BOOST_LOG_TRIVIAL(info) << "udp_dyn_mux_server(" << this << ") bound at " << socket.local_endpoint();
      state = running;
      start_keepalive_timer();
      schedule_read();
      handler(gh::error_code());
    } catch (const gh::system_error& e) {
      BOOST_LOG_TRIVIAL(error) << "udp_dyn_mux_server(" << this << ") start failed: " << e.what();
      state = error;
      handler(e.code());
    }
    break;
  case running:
    handler(gh::error_code());
    break;
  default:
    handler(gh::error_code{app_error_category::incorrect_state, app_error});
    break;
  }
}

void udp_dyn_mux_server::schedule_read() {
  if (state != running || read_pending) {
    return;
  }
  read_pending = true;

  auto a = std::make_shared<std::array<uint8_t, 2048>>();
  auto p = packet{buffer(*a), a};
  auto buffer = boost::asio::mutable_buffer{p.first};
  auto peer_address = std::make_shared<boost::asio::ip::udp::endpoint>();

  socket.async_receive_from(
      buffer, *peer_address,
      [me = shared_from_this(), p{std::move(p)}, peer_address](const gh::error_code& ec,
                                                               std::size_t bytes_transferred) mutable {
        me->read_pending = false;
        if (!ec) {
          auto& buf = p.first;
          assert(bytes_transferred <= buf.capacity - buf.offset);
          buf.length = bytes_transferred;

          if (bytes_transferred >= 2) {
            uint16_t channel_id = (buf.data[buf.offset] << 8) | buf.data[buf.offset + 1];
            auto now = std::chrono::steady_clock::now();

            if (channel_id == 0) {
              // Control packet
              if (bytes_transferred >= 3) {
                uint8_t msg_type = buf.data[buf.offset + 2];
                if (msg_type == udp_dyn_mux::msg_type::client_req_id) {
                  if (auto req = udp_dyn_mux::client_req_id::deserialize(buf.data + buf.offset, bytes_transferred)) {
                    uint32_t cookie = req->cookie;

                    // Check if this endpoint already has a session
                    auto it = std::ranges::find_if(
                        me->clients, [peer_address](const auto& pair) { return pair.second.peer == *peer_address; });
                    if (it != me->clients.end()) {
                      auto& c = it->second;
                      c.cookie = cookie;
                      c.last_seen = now;
                      c.missing_acks = 0;
                      BOOST_LOG_TRIVIAL(info) << "udp_dyn_mux_server(" << &*me << ") re-assigning ID: " << it->first
                                              << " to peer: " << *peer_address;
                      me->send_control_assign(*peer_address, cookie, it->first);
                    } else {
                      // Find a free channel ID dynamically
                      uint16_t free_id = 0;
                      for (uint16_t candidate = 1; candidate < 65535; ++candidate) {
                        if (me->clients.find(candidate) == me->clients.end()) {
                          free_id = candidate;
                          break;
                        }
                      }

                      if (free_id != 0) {
                        auto [it, ok] = me->clients.emplace(
                            free_id, client_session{.peer = *peer_address, .cookie = cookie, .last_seen = now});
                        assert(ok);
                        BOOST_LOG_TRIVIAL(info) << "udp_dyn_mux_server(" << &*me << ") assigned dynamic ID: " << free_id
                                                << " to peer: " << *peer_address;
                        me->send_control_assign(*peer_address, cookie, free_id);
                      } else {
                        BOOST_LOG_TRIVIAL(warning)
                            << "udp_dyn_mux_server(" << &*me << ") allocation failed: no free channels";
                        // TODO: need an error mesaage rather than id 0 channel
                        me->send_control_assign(*peer_address, cookie, 0);
                      }
                    }
                  }
                } else if (msg_type == udp_dyn_mux::msg_type::client_keepalive_ack) {
                  if (auto ack =
                          udp_dyn_mux::client_keepalive_ack::deserialize(buf.data + buf.offset, bytes_transferred)) {
                    uint16_t id = ack->id;
                    auto peer_it = me->clients.find(id);
                    if (peer_it != me->clients.end()) {
                      auto& c = peer_it->second;
                      if (c.peer == *peer_address) {
                        c.missing_acks = 0;
                        c.last_seen = now;
                      } else {
                        me->send_control_addr_mismatch(*peer_address, id);
                      }
                    } else {
                      me->send_control_invalid_id(*peer_address, id);
                    }
                  }
                } else if (msg_type == udp_dyn_mux::msg_type::client_addr_migrate) {
                  if (auto migrate =
                          udp_dyn_mux::client_addr_migrate::deserialize(buf.data + buf.offset, bytes_transferred)) {
                    uint16_t id = migrate->id;
                    uint32_t cookie = migrate->cookie;

                    auto peer_it = me->clients.find(id);
                    if (peer_it != me->clients.end()) {
                      auto& c = peer_it->second;
                      if (c.cookie == cookie) {
                        BOOST_LOG_TRIVIAL(info) << "udp_dyn_mux_server(" << &*me << ") migrating ID " << id << " from "
                                                << c.peer << " to " << *peer_address;
                        c.peer = *peer_address;
                        c.last_seen = now;
                        c.missing_acks = 0;
                        me->send_control_migrate_ack(*peer_address, id);
                      } else {
                        BOOST_LOG_TRIVIAL(warning)
                            << "udp_dyn_mux_server(" << &*me << ") migration cookie mismatch for ID " << id << " from "
                            << c.peer << " to " << *peer_address << "; cookie: " << c.cookie << " != " << cookie;
                        me->send_control_cookie_mismatch(*peer_address, id);
                      }
                    } else {
                      me->send_control_invalid_id(*peer_address, id);
                    }
                  }
                }
              }
            } else {
              // Data packet
              auto peer_it = me->clients.find(channel_id);
              if (peer_it != me->clients.end()) {
                auto& c = peer_it->second;
                if (c.peer == *peer_address) {
                  c.missing_acks = 0;
                  c.last_seen = now;
                  BOOST_LOG_TRIVIAL(debug) << "udp_dyn_mux_server(" << &*me << ") data packet for channel ID "
                                           << channel_id << " ignored (no pipeline/handler bound)";
                } else {
                  me->send_control_addr_mismatch(*peer_address, channel_id);
                }
              } else {
                me->send_control_invalid_id(*peer_address, channel_id);
              }
            }
          }
        } else {
          if (ec != boost::asio::error::operation_aborted) {
            BOOST_LOG_TRIVIAL(error) << "udp_dyn_mux_server(" << &*me << ") read error: " << ec.message();
          }
        }
        me->schedule_read();
      });
}

void udp_dyn_mux_server::start_keepalive_timer() {
  keepalive_timer.expires_after(std::chrono::seconds(5));
  keepalive_timer.async_wait([me = shared_from_this()](const gh::error_code& ec) {
    if (ec || me->state != running) {
      return;
    }
    me->handle_keepalive_tick();
    me->start_keepalive_timer();
  });
}

void udp_dyn_mux_server::handle_keepalive_tick() {
  std::erase_if(clients, [](auto& it) { return it.second.missing_acks >= 3; });
  for (auto& [id, c] : clients) {
    c.missing_acks++;
    if (std::chrono::steady_clock::now() > c.last_seen + std::chrono::seconds(5)) {
      send_control_keepalive(c.peer, id);
    }
  }
}

void udp_dyn_mux_server::send_control_assign(const boost::asio::ip::udp::endpoint& peer, uint32_t cookie, uint16_t id) {
  auto buf = std::make_shared<std::array<uint8_t, 9>>();
  udp_dyn_mux::server_assign_id{cookie, id}.serialize(*buf);
  socket.async_send_to(boost::asio::buffer(*buf), peer, [buf](const gh::error_code&, std::size_t) {});
}

void udp_dyn_mux_server::send_control_keepalive(const boost::asio::ip::udp::endpoint& peer, uint16_t id) {
  auto buf = std::make_shared<std::array<uint8_t, 5>>();
  udp_dyn_mux::server_keepalive{id}.serialize(*buf);
  socket.async_send_to(boost::asio::buffer(*buf), peer, [buf](const gh::error_code&, std::size_t) {});
}

void udp_dyn_mux_server::send_control_id_closed(const boost::asio::ip::udp::endpoint& peer, uint16_t id) {
  auto buf = std::make_shared<std::array<uint8_t, 5>>();
  udp_dyn_mux::server_id_closed{id}.serialize(*buf);
  socket.async_send_to(boost::asio::buffer(*buf), peer, [buf](const gh::error_code&, std::size_t) {});
}

bool udp_dyn_mux_server::check_rate_limit(const boost::asio::ip::udp::endpoint& peer) {
  auto now = std::chrono::steady_clock::now();
  auto it = last_error_sent.find(peer);
  if (it != last_error_sent.end() && now - it->second < std::chrono::seconds(1)) {
    return false;
  }
  last_error_sent[peer] = now;
  return true;
}

void udp_dyn_mux_server::send_control_addr_mismatch(const boost::asio::ip::udp::endpoint& peer, uint16_t id) {
  if (!check_rate_limit(peer)) {
    return;
  }

  auto buf = std::make_shared<std::array<uint8_t, 5>>();
  udp_dyn_mux::server_addr_mismatch{id}.serialize(*buf);
  socket.async_send_to(boost::asio::buffer(*buf), peer, [buf](const gh::error_code&, std::size_t) {});
}

void udp_dyn_mux_server::send_control_migrate_ack(const boost::asio::ip::udp::endpoint& peer, uint16_t id) {
  auto buf = std::make_shared<std::array<uint8_t, 5>>();
  udp_dyn_mux::server_migrate_ack{id}.serialize(*buf);
  socket.async_send_to(boost::asio::buffer(*buf), peer, [buf](const gh::error_code&, std::size_t) {});
}

void udp_dyn_mux_server::send_control_invalid_id(const boost::asio::ip::udp::endpoint& peer, uint16_t id) {
  if (!check_rate_limit(peer)) {
    return;
  }

  auto buf = std::make_shared<std::array<uint8_t, 5>>();
  udp_dyn_mux::server_invalid_id{id}.serialize(*buf);
  socket.async_send_to(boost::asio::buffer(*buf), peer, [buf](const gh::error_code&, std::size_t) {});
}

void udp_dyn_mux_server::send_control_cookie_mismatch(const boost::asio::ip::udp::endpoint& peer, uint16_t id) {
  if (!check_rate_limit(peer)) {
    return;
  }

  auto buf = std::make_shared<std::array<uint8_t, 5>>();
  udp_dyn_mux::server_cookie_mismatch{id}.serialize(*buf);
  socket.async_send_to(boost::asio::buffer(*buf), peer, [buf](const gh::error_code&, std::size_t) {});
}
