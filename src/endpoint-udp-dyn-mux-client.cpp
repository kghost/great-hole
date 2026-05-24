#include "endpoint-udp-dyn-mux-client.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/log/trivial.hpp>
#include <cassert>
#include <random>

#include "endpoint-udp-dyn-mux-protocol.hpp"

udp_dyn_mux_client::udp_dyn_mux_client(boost::asio::io_context& io_context, boost::asio::ip::udp::endpoint peer)
    : socket(io_context), peer(peer), local(boost::asio::ip::udp::v6(), 0), negotiate_timer(io_context),
      migrate_timer(io_context), keepalive_check_timer(io_context) {
  std::random_device rd;
  prng.seed(rd());
}

udp_dyn_mux_client::udp_dyn_mux_client(boost::asio::io_context& io_context, boost::asio::ip::udp::endpoint peer,
                                       boost::asio::ip::udp::endpoint local)
    : socket(io_context), peer(peer), local(local), negotiate_timer(io_context), migrate_timer(io_context),
      keepalive_check_timer(io_context) {
  std::random_device rd;
  prng.seed(rd());
}

void udp_dyn_mux_client::async_start(std::move_only_function<event>&& handler) {
  switch (state) {
  case none:
    try {
      socket.open(boost::asio::ip::udp::v6());
      socket.set_option(boost::asio::ip::v6_only(false));
      socket.bind(local);
      BOOST_LOG_TRIVIAL(info) << "udp_dyn_mux_client(" << this << ") bound at " << socket.local_endpoint();
    } catch (const gh::system_error& e) {
      BOOST_LOG_TRIVIAL(error) << "udp_dyn_mux_client(" << this << ") start failed: " << e.what();
      state = error;
      handler(e.code());
      return;
    }

    socket.async_connect(
        peer, [me = shared_from_this(), handler{std::move(handler)}](const gh::error_code& ec) mutable {
          if (ec) {
            BOOST_LOG_TRIVIAL(error) << "udp_dyn_mux_client(" << &*me << ") connect failed: " << ec.message();
            me->state = error;
            handler(ec);
            return;
          }
          me->start_negotiation();
          me->schedule_socket_read();
          handler(gh::error_code());
        });
    break;
  case negotiating:
  case running:
  case migrating:
    handler(gh::error_code());
    break;
  default:
    handler(gh::error_code{app_error_category::incorrect_state, app_error});
    break;
  }
}

void udp_dyn_mux_client::start_negotiation() {
  state = negotiating;
  current_cookie = prng();
  BOOST_LOG_TRIVIAL(info) << "udp_dyn_mux_client(" << this << ") starting negotiation with cookie: " << current_cookie;
  send_negotiate_request();
}

void udp_dyn_mux_client::send_negotiate_request() {
  if (state != negotiating) {
    return;
  }

  auto buf = std::make_shared<std::array<uint8_t, 7>>();
  udp_dyn_mux::client_req_id{current_cookie}.serialize(*buf);

  socket.async_send(boost::asio::buffer(*buf), [me = shared_from_this(), buf](const gh::error_code& ec, std::size_t) {
    // Handled by timer retransmissions
  });

  negotiate_timer.expires_after(std::chrono::seconds(1));
  negotiate_timer.async_wait([me = shared_from_this()](const gh::error_code& ec) {
    if (!ec && me->state == negotiating) {
      BOOST_LOG_TRIVIAL(info) << "udp_dyn_mux_client(" << &*me << ") retrying negotiation request...";
      me->send_negotiate_request();
    }
  });
}

void udp_dyn_mux_client::start_migration() {
  state = migrating;
  BOOST_LOG_TRIVIAL(info) << "udp_dyn_mux_client(" << this << ") starting address migration for ID: " << assigned_id;
  send_migration_request();
}

void udp_dyn_mux_client::send_migration_request() {
  if (state != migrating) {
    return;
  }

  auto buf = std::make_shared<std::array<uint8_t, 9>>();
  udp_dyn_mux::client_addr_migrate{assigned_id, current_cookie}.serialize(*buf);

  socket.async_send(boost::asio::buffer(*buf), [me = shared_from_this(), buf](const gh::error_code& ec, std::size_t) {
    // Handled by timer retransmissions
  });

  migrate_timer.expires_after(std::chrono::seconds(1));
  migrate_timer.async_wait([me = shared_from_this()](const gh::error_code& ec) {
    if (!ec && me->state == migrating) {
      BOOST_LOG_TRIVIAL(info) << "udp_dyn_mux_client(" << &*me << ") retrying migration request...";
      me->send_migration_request();
    }
  });
}

void udp_dyn_mux_client::check_keepalive_timeout() {
  keepalive_check_timer.expires_after(std::chrono::seconds(5));
  keepalive_check_timer.async_wait([me = shared_from_this()](const gh::error_code& ec) {
    if (ec || me->state != running) {
      return;
    }
    auto now = std::chrono::steady_clock::now();
    if (now - me->last_keepalive_received > std::chrono::seconds(15)) {
      BOOST_LOG_TRIVIAL(warning) << "udp_dyn_mux_client(" << &*me
                                 << ") keepalive timeout from server, re-negotiating...";
      me->start_negotiation();
    } else {
      me->check_keepalive_timeout();
    }
  });
}

void udp_dyn_mux_client::schedule_socket_read() {
  if (state == error || state == none) {
    return;
  }

  auto a = std::make_shared<std::array<uint8_t, 2048>>();
  auto p = packet{buffer(*a), a};
  auto buffer = boost::asio::mutable_buffer{p.first};

  socket.async_receive(buffer, [me = shared_from_this(), p{std::move(p)}](const gh::error_code& ec,
                                                                          std::size_t bytes_transferred) mutable {
    if (ec) {
      if (ec != boost::asio::error::operation_aborted) {
        BOOST_LOG_TRIVIAL(error) << "udp_dyn_mux_client(" << &*me << ") read error: " << ec.message();
        me->state = error;
        if (me->read_handler_cb) {
          auto h = std::move(me->read_handler_cb);
          me->read_handler_cb = nullptr;
          auto a_dummy = std::make_shared<std::array<uint8_t, 0>>();
          h(ec, packet{::buffer(*a_dummy), a_dummy});
        }
      }
      return;
    }

    auto& buf = p.first;
    assert(bytes_transferred <= buf.capacity - buf.offset);
    buf.length = bytes_transferred;

    if (bytes_transferred >= 2) {
      uint16_t channel_id = (buf.data[buf.offset] << 8) | buf.data[buf.offset + 1];
      if (channel_id == 0) {
        me->handle_control_packet(buf.data + buf.offset, buf.length);
      } else if (me->state == running && channel_id == me->assigned_id) {
        buf.offset += 2;
        buf.length -= 2;

        if (me->read_handler_cb) {
          auto h = std::move(me->read_handler_cb);
          me->read_handler_cb = nullptr;
          h(gh::error_code(), std::move(p));
        } else {
          if (me->incoming_data_queue.size() < 1000) {
            me->incoming_data_queue.push(std::move(p));
          } else {
            BOOST_LOG_TRIVIAL(warning) << "udp_dyn_mux_client(" << &*me
                                       << ") incoming data queue full, dropping packet";
          }
        }
      } else {
        BOOST_LOG_TRIVIAL(debug) << "udp_dyn_mux_client(" << &*me
                                 << ") ignored data packet for unknown channel ID: " << channel_id;
      }
    }
    me->schedule_socket_read();
  });
}

void udp_dyn_mux_client::handle_control_packet(const uint8_t* data, std::size_t length) {
  if (length < 3) {
    return;
  }

  uint8_t msg_type = data[2];
  if (msg_type == udp_dyn_mux::msg_type::server_assign_id) {
    if (auto assign = udp_dyn_mux::server_assign_id::deserialize(data, length)) {
      if (state == negotiating && assign->cookie == current_cookie) {
        negotiate_timer.cancel();
        assigned_id = assign->assigned_id;
        if (assigned_id == 0) {
          BOOST_LOG_TRIVIAL(warning) << "udp_dyn_mux_client(" << this << ") negotiation failed: no free IDs";
          negotiate_timer.expires_after(std::chrono::seconds(5));
          negotiate_timer.async_wait([me = shared_from_this()](const gh::error_code& ec) {
            if (!ec) {
              me->start_negotiation();
            }
          });
        } else {
          state = running;
          last_keepalive_received = std::chrono::steady_clock::now();
          BOOST_LOG_TRIVIAL(info) << "udp_dyn_mux_client(" << this << ") successfully negotiated ID: " << assigned_id;
          check_keepalive_timeout();
          flush_write_queue();
        }
      }
    }
  } else if (msg_type == udp_dyn_mux::msg_type::server_keepalive) {
    if (auto keepalive = udp_dyn_mux::server_keepalive::deserialize(data, length)) {
      uint16_t id = keepalive->id;
      if (state == running && id == assigned_id) {
        last_keepalive_received = std::chrono::steady_clock::now();
        auto ack = std::make_shared<std::array<uint8_t, 5>>();
        udp_dyn_mux::client_keepalive_ack{assigned_id}.serialize(*ack);
        socket.async_send(boost::asio::buffer(*ack), [ack](const gh::error_code&, std::size_t) {});
      }
    }
  } else if (msg_type == udp_dyn_mux::msg_type::server_id_closed) {
    if (auto closed = udp_dyn_mux::server_id_closed::deserialize(data, length)) {
      uint16_t id = closed->id;
      if (state == running && id == assigned_id) {
        BOOST_LOG_TRIVIAL(warning) << "udp_dyn_mux_client(" << this << ") server closed ID, re-negotiating...";
        start_negotiation();
      }
    }
  } else if (msg_type == udp_dyn_mux::msg_type::server_addr_mismatch) {
    if (auto mismatch = udp_dyn_mux::server_addr_mismatch::deserialize(data, length)) {
      uint16_t id = mismatch->id;
      if (state == running && id == assigned_id) {
        BOOST_LOG_TRIVIAL(warning) << "udp_dyn_mux_client(" << this
                                   << ") server detected source address mismatch, migrating...";
        start_migration();
      }
    }
  } else if (msg_type == udp_dyn_mux::msg_type::server_migrate_ack) {
    if (auto migrate_ack = udp_dyn_mux::server_migrate_ack::deserialize(data, length)) {
      uint16_t id = migrate_ack->id;
      if (state == migrating && id == assigned_id) {
        migrate_timer.cancel();
        state = running;
        last_keepalive_received = std::chrono::steady_clock::now();
        BOOST_LOG_TRIVIAL(info) << "udp_dyn_mux_client(" << this << ") successfully migrated ID: " << assigned_id;
        check_keepalive_timeout();
        flush_write_queue();
      }
    }
  } else if (msg_type == udp_dyn_mux::msg_type::server_invalid_id) {
    if (auto invalid_id = udp_dyn_mux::server_invalid_id::deserialize(data, length)) {
      uint16_t id = invalid_id->id;
      if (state == running && id == assigned_id) {
        BOOST_LOG_TRIVIAL(warning) << "udp_dyn_mux_client(" << this
                                   << ") server reported invalid ID, re-negotiating...";
        start_negotiation();
      }
    }
  } else if (msg_type == udp_dyn_mux::msg_type::server_cookie_mismatch) {
    if (auto cookie_mismatch = udp_dyn_mux::server_cookie_mismatch::deserialize(data, length)) {
      uint16_t id = cookie_mismatch->id;
      if (state == migrating && id == assigned_id) {
        BOOST_LOG_TRIVIAL(warning) << "udp_dyn_mux_client(" << this
                                   << ") server reported migration cookie mismatch, re-negotiating...";
        migrate_timer.cancel();
        start_negotiation();
      }
    }
  }
}

void udp_dyn_mux_client::async_read(std::move_only_function<read_handler>&& handler) {
  if (state == error) {
    auto a_dummy = std::make_shared<std::array<uint8_t, 1>>();
    handler(gh::error_code{app_error_category::incorrect_state, app_error}, packet{buffer(*a_dummy), a_dummy});
    return;
  }

  if (!incoming_data_queue.empty()) {
    auto p = std::move(incoming_data_queue.front());
    incoming_data_queue.pop();
    handler(gh::error_code(), std::move(p));
  } else {
    assert(!read_handler_cb);
    read_handler_cb = std::move(handler);
  }
}

void udp_dyn_mux_client::async_write(packet&& p, std::move_only_function<write_handler>&& handler) {
  if (state == error) {
    handler(gh::error_code{app_error_category::incorrect_state, app_error}, 0);
    return;
  }

  if (state == none || state == negotiating || state == migrating) {
    write_queue.push(std::make_pair(std::move(p), std::move(handler)));
    return;
  }

  assert(!write_pending);
  write_pending = true;

  auto& buf = p.first;
  if (buf.offset < 2) {
    handler(gh::error_code{app_error_category::invalid_packet_reserved, app_error}, 0);
    write_pending = false;
    return;
  }

  buf.offset -= 2;
  buf.length += 2;
  buf.data[buf.offset] = (assigned_id >> 8) & 0xFF;
  buf.data[buf.offset + 1] = assigned_id & 0xFF;

  socket.async_send(boost::asio::const_buffer{buf},
                    [me = shared_from_this(), handler{std::move(handler)}](const gh::error_code& ec,
                                                                           std::size_t bytes_transferred) mutable {
                      me->write_pending = false;
                      handler(ec, bytes_transferred);
                      me->flush_write_queue();
                    });
}

void udp_dyn_mux_client::flush_write_queue() {
  if (state != running || write_queue.empty() || write_pending) {
    return;
  }
  auto p = std::move(write_queue.front());
  write_queue.pop();
  async_write(std::move(p.first), std::move(p.second));
}
