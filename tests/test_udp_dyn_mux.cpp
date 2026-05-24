#include <boost/asio.hpp>
#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <random>

#include "endpoint-udp-dyn-mux-client.hpp"
#include "endpoint-udp-dyn-mux-server.hpp"
#include "error-code.hpp"

using boost::asio::ip::udp;

#if 0 // Temporarily disabled by user request to clean up test-only code from server and standardise functionality

TEST(UdpDynMuxTest, SuccessfulNegotiationAndDataTransfer) {
  boost::asio::io_context io_context;

  // Start server on ephemeral port
  auto server = std::make_shared<udp_dyn_mux_server>(io_context, udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  server->test_register_id(1);

  bool server_started = false;
  server->async_start([&](const gh::error_code& ec) {
    ASSERT_FALSE(ec);
    server_started = true;
  });

  // Start client connecting to server
  io_context.poll(); // Let server bind
  auto client = std::make_shared<udp_dyn_mux_client>(io_context, server->local_endpoint(),
                                                     udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));

  bool client_started = false;
  client->async_start([&](const gh::error_code& ec) {
    ASSERT_FALSE(ec);
    client_started = true;
  });

  // Verify server & client started
  io_context.restart();
  io_context.run_for(std::chrono::milliseconds(200));
  ASSERT_TRUE(server_started);
  ASSERT_TRUE(client_started);

  // Send packet client -> server
  bool server_received = false;
  server->read(1, [&](const gh::error_code& ec, packet&& p) {
    ASSERT_FALSE(ec);
    server_received = true;
    ASSERT_EQ(p.first.length, 5);
    ASSERT_EQ(std::string((char*)p.first.data + p.first.offset, p.first.length), "Hello");
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

  auto a2 = std::make_shared<std::array<uint8_t, 2048>>();
  auto p2 = packet{buffer(*a2), a2};
  std::memcpy(p2.first.data + p2.first.offset, "World", 5);
  p2.first.length = 5;

  server->write(1, std::move(p2), [&](const gh::error_code& ec, std::size_t n) {
    ASSERT_FALSE(ec);
    ASSERT_EQ(n, 7);
  });

  io_context.restart();
  io_context.run_for(std::chrono::milliseconds(200));
  EXPECT_TRUE(client_received);
}

TEST(UdpDynMuxTest, PacketQueueingDuringNegotiation) {
  boost::asio::io_context io_context;

  auto server = std::make_shared<udp_dyn_mux_server>(io_context, udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  server->test_register_id(1);

  server->async_start([&](const gh::error_code&) {});

  io_context.poll();
  auto client = std::make_shared<udp_dyn_mux_client>(io_context, server->local_endpoint(),
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

  bool server_received = false;
  server->read(1, [&](const gh::error_code& ec, packet&& p) {
    ASSERT_FALSE(ec);
    server_received = true;
    ASSERT_EQ(p.first.length, 6);
    ASSERT_EQ(std::string((char*)p.first.data + p.first.offset, p.first.length), "Queued");
  });

  // Start client to trigger negotiation
  client->async_start([&](const gh::error_code&) {});

  io_context.restart();
  io_context.run_for(std::chrono::milliseconds(300));

  EXPECT_TRUE(write_completed);
  EXPECT_TRUE(server_received);
}

TEST(UdpDynMuxTest, AddressMigrationAndCookieMismatch) {
  boost::asio::io_context io_context;

  auto server = std::make_shared<udp_dyn_mux_server>(io_context, udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  server->test_register_id(1);
  server->async_start([&](const gh::error_code&) {});

  bool server_received = false;
  server->read(1, [&](const gh::error_code& ec, packet&& p) {
    ASSERT_FALSE(ec);
    server_received = true;
    ASSERT_EQ(p.first.length, 5);
    ASSERT_EQ(std::string((char*)p.first.data + p.first.offset, p.first.length), "Migra");
  });

  io_context.poll();

  // Create client
  auto client = std::make_shared<udp_dyn_mux_client>(io_context, server->local_endpoint(),
                                                     udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  client->async_start([&](const gh::error_code&) {});

  io_context.restart();
  io_context.run_for(std::chrono::milliseconds(200));

  // Retrieve assigned ID from client
  uint16_t assigned_id = client->get_assigned_id();
  ASSERT_EQ(assigned_id, 1);

  // Send packet to trigger migration once socket is rebound
  auto a = std::make_shared<std::array<uint8_t, 2048>>();
  auto p = packet{buffer(*a), a};
  std::memcpy(p.first.data + p.first.offset, "Migra", 5);
  p.first.length = 5;

  // Rebind the client's socket to simulate migrating IP/port
  client->test_rebind(udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));

  bool write_completed = false;
  client->async_write(std::move(p), [&](const gh::error_code& ec, std::size_t n) {
    ASSERT_FALSE(ec);
    write_completed = true;
  });

  // Run event loop to process address migration
  io_context.restart();
  io_context.run_for(std::chrono::milliseconds(800));

  EXPECT_TRUE(write_completed);

  // Send a new packet after migration is fully completed to verify data routing
  auto a_mig = std::make_shared<std::array<uint8_t, 2048>>();
  auto p_mig = packet{buffer(*a_mig), a_mig};
  std::memcpy(p_mig.first.data + p_mig.first.offset, "Migra", 5);
  p_mig.first.length = 5;

  bool migrated_write_success = false;
  client->async_write(std::move(p_mig), [&](const gh::error_code& ec, std::size_t n) {
    ASSERT_FALSE(ec);
    migrated_write_success = true;
  });

  io_context.restart();
  io_context.run_for(std::chrono::milliseconds(300));

  EXPECT_TRUE(migrated_write_success);
  EXPECT_TRUE(server_received);
}

#endif
