#include <boost/asio.hpp>
#include <chrono>
#include <gtest/gtest.h>
#include <memory>

#include "endpoint-udp-mux-client.hpp"
#include "endpoint-udp-mux-server.hpp"
#include "endpoint.hpp"
#include "error-code.hpp"
#include "pipeline.hpp"

using boost::asio::ip::udp;

TEST(UdpMuxServerTest, IgnoresEmptyPacket) {
  boost::asio::io_context io_context;

  // Start server on an ephemeral port
  auto server = std::make_shared<udp_mux_server>(io_context, udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  auto channel = server->create_channel(42);

  bool server_started = false;
  std::static_pointer_cast<endpoint_input>(channel)->async_start([&](const gh::error_code& ec) {
    ASSERT_FALSE(ec);
    server_started = true;
  });

  bool packet_received = false;
  channel->async_read([&](const gh::error_code& ec, packet&& p) {
    ASSERT_FALSE(ec);
    packet_received = true;
    ASSERT_EQ(p.first.length, 5); // 5 bytes payload
  });

  // Create a plain UDP socket to send raw packets
  udp::socket raw_socket(io_context, udp::v6());

  // Send a 0-byte packet
  raw_socket.send_to(boost::asio::buffer("", 0), server->local_endpoint());

  // Send a valid packet for channel 42
  uint8_t valid_payload[] = {42, 'H', 'e', 'l', 'l', 'o'};
  raw_socket.send_to(boost::asio::buffer(valid_payload), server->local_endpoint());

  io_context.restart();
  io_context.run_for(std::chrono::milliseconds(500));

  EXPECT_TRUE(packet_received);
}

TEST(UdpMuxClientTest, IgnoresStrayPackets) {
  boost::asio::io_context io_context;

  // Create a plain UDP socket to send raw packets
  udp::socket raw_socket(io_context, udp::v6());
  raw_socket.bind(udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  udp::endpoint peer_ep = raw_socket.local_endpoint();

  auto client = std::make_shared<udp_mux_client>(io_context, 42, peer_ep,
                                                 udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  bool client_started = false;
  client->async_start([&](const gh::error_code& ec) {
    ASSERT_FALSE(ec);
    client_started = true;
  });

  io_context.run_one();
  ASSERT_TRUE(client_started);

  bool packet_received = false;
  client->async_read([&](const gh::error_code& ec, packet&& p) {
    ASSERT_FALSE(ec);
    packet_received = true;
    ASSERT_EQ(p.first.length, 5); // 5 bytes payload
  });

  raw_socket.connect(client->local_endpoint());

  // Send a 0-byte packet
  // Send a 0-byte packet
  raw_socket.send(boost::asio::buffer("", 0));

  // Send a packet with wrong channel ID (99)
  uint8_t wrong_channel_payload[] = {99, 'B', 'a', 'd'};
  raw_socket.send(boost::asio::buffer(wrong_channel_payload));

  // Send a valid packet for channel 42
  uint8_t valid_payload[] = {42, 'H', 'e', 'l', 'l', 'o'};
  raw_socket.send(boost::asio::buffer(valid_payload));

  io_context.restart();
  io_context.run_for(std::chrono::milliseconds(500));

  EXPECT_TRUE(packet_received);
}

// Mock endpoint that fails reads with a non-critical error
class MockErrorEndpoint : public endpoint_input,
                          public endpoint_output,
                          public std::enable_shared_from_this<MockErrorEndpoint> {
public:
  int error_count = 0;
  int success_count = 0;

  void async_start(std::move_only_function<event>&& handler) override { handler(gh::error_code()); }

  void async_read(std::move_only_function<read_handler>&& handler) override {
    if (error_count == 0) {
      error_count++;
      // Simulate a non-critical error
      auto a = std::make_shared<std::array<uint8_t, 1>>();
      auto p = packet{buffer(*a), a};
      handler(gh::error_code{app_error_category::invalid_packet_session, app_error}, std::move(p));
    } else if (success_count == 0) {
      success_count++;
      // Simulate success
      auto a = std::make_shared<std::array<uint8_t, 2048>>();
      auto p = packet{buffer(*a), a};
      handler(gh::error_code(), std::move(p));
    }
  }

  void async_write(packet&& p, std::move_only_function<write_handler>&& handler) override {
    handler(gh::error_code(), p.first.length);
  }
};

TEST(PipelineTest, RecoversFromNonCriticalReadError) {
  auto mock_ep = std::make_shared<MockErrorEndpoint>();

  auto pipe = std::make_shared<pipeline>(mock_ep, std::vector<std::shared_ptr<filter>>{}, mock_ep);
  pipe->start();

  // When pipeline reads, it should first get an error, then a success, and continue.
  // If it doesn't stop on the error, success_count will become 1.
  EXPECT_EQ(mock_ep->error_count, 1);
  EXPECT_EQ(mock_ep->success_count, 1);

  // We can also check that pipeline state is still running
  // The pipeline state is public? No, we just know it called read twice.
}
