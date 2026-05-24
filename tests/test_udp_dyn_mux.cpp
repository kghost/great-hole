#include <boost/asio.hpp>
#include <chrono>
#include <functional>
#include <gtest/gtest.h>
#include <memory>

#include "endpoint-udp-dyn-mux-client.hpp"
#include "endpoint-udp-dyn-mux-protocol.hpp"
#include "endpoint-udp-dyn-mux-server.hpp"
#include "error-code.hpp"

using boost::asio::ip::udp;

TEST(UdpDynMuxTest, SuccessfulNegotiationAndDataTransfer) {
  boost::asio::io_context io_context;

  // Start raw UDP socket to act as the server
  udp::socket server_socket(io_context, udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  udp::endpoint server_ep = server_socket.local_endpoint();

  // Start client connecting to our mock server
  auto client = std::make_shared<udp_dyn_mux_client>(io_context, server_ep,
                                                     udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));

  bool client_started = false;
  client->async_start([&](const gh::error_code& ec) {
    ASSERT_FALSE(ec);
    client_started = true;
  });

  // Server: listen for negotiation request
  uint8_t rx_buf[2048];
  udp::endpoint client_ep;
  bool negotiation_request_received = false;
  uint32_t client_cookie = 0;

  server_socket.async_receive_from(boost::asio::buffer(rx_buf), client_ep,
                                   [&](const boost::system::error_code& ec, std::size_t n) {
                                     ASSERT_FALSE(ec);
                                     auto req = udp_dyn_mux::client_req_id::deserialize(rx_buf, n);
                                     ASSERT_TRUE(req.has_value());
                                     client_cookie = req->cookie;
                                     negotiation_request_received = true;

                                     // Send back assignment
                                     uint8_t tx_buf[udp_dyn_mux::server_assign_id::size];
                                     udp_dyn_mux::server_assign_id{client_cookie, 1}.serialize(tx_buf);
                                     server_socket.send_to(boost::asio::buffer(tx_buf), client_ep);
                                   });

  // Run until negotiation is complete
  io_context.restart();
  io_context.run_for(std::chrono::milliseconds(200));
  ASSERT_TRUE(client_started);
  ASSERT_TRUE(negotiation_request_received);

  // Send packet client -> server
  bool server_received = false;
  server_socket.async_receive_from(boost::asio::buffer(rx_buf), client_ep,
                                   [&](const boost::system::error_code& ec, std::size_t n) {
                                     ASSERT_FALSE(ec);
                                     ASSERT_GE(n, 2);
                                     uint16_t id = udp_dyn_mux::read_uint16_be(rx_buf);
                                     ASSERT_EQ(id, 1);
                                     ASSERT_EQ(std::string((char*)rx_buf + 2, n - 2), "Hello");
                                     server_received = true;
                                   });

  auto a = std::make_shared<std::array<uint8_t, 2048>>();
  auto p = packet{buffer(*a), a};
  std::memcpy(p.first.data + p.first.offset, "Hello", 5);
  p.first.length = 5;

  client->async_write(std::move(p), [&](const gh::error_code& ec, std::size_t n) {
    ASSERT_FALSE(ec);
    ASSERT_EQ(n, 7); // 5 bytes payload + 2 bytes header
  });

  io_context.restart();
  io_context.run_for(std::chrono::milliseconds(200));
  EXPECT_TRUE(server_received);

  // Send packet server -> client
  bool client_received = false;
  client->async_read([&](const gh::error_code& ec, packet&& p) {
    ASSERT_FALSE(ec);
    client_received = true;
    ASSERT_EQ(p.first.length, 5);
    ASSERT_EQ(std::string((char*)p.first.data + p.first.offset, p.first.length), "World");
  });

  uint8_t data_pkg[7];
  udp_dyn_mux::write_uint16_be(data_pkg, 1);
  std::memcpy(data_pkg + 2, "World", 5);
  server_socket.send_to(boost::asio::buffer(data_pkg), client_ep);

  io_context.restart();
  io_context.run_for(std::chrono::milliseconds(200));
  EXPECT_TRUE(client_received);
}

TEST(UdpDynMuxTest, PacketQueueingDuringNegotiation) {
  boost::asio::io_context io_context;

  udp::socket server_socket(io_context, udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  udp::endpoint server_ep = server_socket.local_endpoint();

  auto client = std::make_shared<udp_dyn_mux_client>(io_context, server_ep,
                                                     udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));

  // Write packet BEFORE client async_start (so it's queued)
  auto a = std::make_shared<std::array<uint8_t, 2048>>();
  auto p = packet{buffer(*a), a};
  std::memcpy(p.first.data + p.first.offset, "Queued", 6);
  p.first.length = 6;

  bool write_completed = false;
  client->async_write(std::move(p), [&](const gh::error_code& ec, std::size_t n) {
    ASSERT_FALSE(ec);
    write_completed = true;
  });

  // Start client to trigger negotiation
  client->async_start([&](const gh::error_code&) {});

  // Server handles negotiation and then receives the queued packet
  uint8_t rx_buf[2048];
  udp::endpoint client_ep;
  bool negotiation_handled = false;
  bool packet_received = false;

  std::function<void()> handle_server = [&]() {
    server_socket.async_receive_from(boost::asio::buffer(rx_buf), client_ep,
                                     [&](const boost::system::error_code& ec, std::size_t n) {
                                       ASSERT_FALSE(ec);
                                       if (!negotiation_handled) {
                                         auto req = udp_dyn_mux::client_req_id::deserialize(rx_buf, n);
                                         ASSERT_TRUE(req.has_value());
                                         uint8_t tx_buf[udp_dyn_mux::server_assign_id::size];
                                         udp_dyn_mux::server_assign_id{req->cookie, 1}.serialize(tx_buf);
                                         server_socket.send_to(boost::asio::buffer(tx_buf), client_ep);
                                         negotiation_handled = true;
                                         // Look for the queued data packet next
                                         handle_server();
                                       } else {
                                         ASSERT_GE(n, 2);
                                         uint16_t id = udp_dyn_mux::read_uint16_be(rx_buf);
                                         ASSERT_EQ(id, 1);
                                         ASSERT_EQ(std::string((char*)rx_buf + 2, n - 2), "Queued");
                                         packet_received = true;
                                       }
                                     });
  };
  handle_server();

  io_context.restart();
  io_context.run_for(std::chrono::milliseconds(400));

  EXPECT_TRUE(write_completed);
  EXPECT_TRUE(packet_received);
}

TEST(UdpDynMuxTest, ServerHandlesMigration) {
  boost::asio::io_context io_context;

  auto server =
      std::make_shared<udp_dyn_mux_server>(io_context, udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  bool server_started = false;
  server->async_start([&](const gh::error_code& ec) {
    ASSERT_FALSE(ec);
    server_started = true;
  });

  io_context.restart();
  io_context.run_for(std::chrono::milliseconds(100));
  ASSERT_TRUE(server_started);

  udp::endpoint server_ep = server->local_endpoint();

  // Mock client socket 1
  udp::socket client1(io_context, udp::endpoint(udp::v6(), 0));
  uint32_t cookie = 0x12345678;

  uint8_t tx_buf[2048];
  udp_dyn_mux::client_req_id{cookie}.serialize(tx_buf);
  client1.send_to(boost::asio::buffer(tx_buf, udp_dyn_mux::client_req_id::size), server_ep);

  io_context.restart();
  io_context.run_for(std::chrono::milliseconds(100));

  // Receive assignment
  uint8_t rx_buf[2048];
  udp::endpoint sender_ep;
  size_t n = client1.receive_from(boost::asio::buffer(rx_buf), sender_ep);
  auto assign = udp_dyn_mux::server_assign_id::deserialize(rx_buf, n);
  ASSERT_TRUE(assign.has_value());
  ASSERT_EQ(assign->cookie, cookie);
  uint16_t id = assign->assigned_id;

  // Now "migrate" to client socket 2
  udp::socket client2(io_context, udp::endpoint(udp::v6(), 0));
  udp_dyn_mux::client_addr_migrate{id, cookie}.serialize(tx_buf);
  client2.send_to(boost::asio::buffer(tx_buf, udp_dyn_mux::client_addr_migrate::size), server_ep);

  io_context.restart();
  io_context.run_for(std::chrono::milliseconds(100));

  // Receive migration ACK on client 2
  n = client2.receive_from(boost::asio::buffer(rx_buf), sender_ep);
  auto migrate_ack = udp_dyn_mux::server_migrate_ack::deserialize(rx_buf, n);
  ASSERT_TRUE(migrate_ack.has_value());
  ASSERT_EQ(migrate_ack->id, id);
}
