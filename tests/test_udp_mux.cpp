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
using namespace gh;

TEST(UdpMuxServerTest, IgnoresEmptyPacket) {
  boost::asio::io_context ioContext;

  // Start server on an ephemeral port
  auto server = std::make_shared<UdpMuxServer>(ioContext, udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  auto channel = server->CreateChannel(42);

  bool serverStarted = false;
  std::static_pointer_cast<EndpointInput>(channel)->AsyncStart([&](const ErrorCode& ec) {
    ASSERT_FALSE(ec);
    serverStarted = true;
  });

  bool packetReceived = false;
  channel->AsyncRead([&](const ErrorCode& ec, Packet&& p) {
    ASSERT_FALSE(ec);
    packetReceived = true;
    ASSERT_EQ(p.first.Length, 5); // 5 bytes payload
  });

  // Create a plain UDP socket to send raw packets
  udp::socket rawSocket(ioContext, udp::v6());

  // Send a 0-byte packet
  rawSocket.send_to(boost::asio::buffer("", 0), server->LocalEndpoint());

  // Send a valid packet for channel 42
  uint8_t validPayload[] = {42, 'H', 'e', 'l', 'l', 'o'};
  rawSocket.send_to(boost::asio::buffer(validPayload), server->LocalEndpoint());

  ioContext.restart();
  ioContext.run_for(std::chrono::milliseconds(500));

  EXPECT_TRUE(packetReceived);
}

TEST(UdpMuxClientTest, IgnoresStrayPackets) {
  boost::asio::io_context ioContext;

  // Create a plain UDP socket to send raw packets
  udp::socket rawSocket(ioContext, udp::v6());
  rawSocket.bind(udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  udp::endpoint peerEp = rawSocket.local_endpoint();

  auto client =
      std::make_shared<UdpMuxClient>(ioContext, 42, peerEp, udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  bool clientStarted = false;
  client->AsyncStart([&](const ErrorCode& ec) {
    ASSERT_FALSE(ec);
    clientStarted = true;
  });

  ioContext.run_one();
  ASSERT_TRUE(clientStarted);

  bool packetReceived = false;
  client->AsyncRead([&](const ErrorCode& ec, Packet&& p) {
    ASSERT_FALSE(ec);
    packetReceived = true;
    ASSERT_EQ(p.first.Length, 5); // 5 bytes payload
  });

  rawSocket.connect(client->LocalEndpoint());

  // Send a 0-byte packet
  rawSocket.send(boost::asio::buffer("", 0));

  // Send a packet with wrong channel ID (99)
  uint8_t wrongChannelPayload[] = {99, 'B', 'a', 'd'};
  rawSocket.send(boost::asio::buffer(wrongChannelPayload));

  // Send a valid packet for channel 42
  uint8_t validPayload[] = {42, 'H', 'e', 'l', 'l', 'o'};
  rawSocket.send(boost::asio::buffer(validPayload));

  ioContext.restart();
  ioContext.run_for(std::chrono::milliseconds(500));

  EXPECT_TRUE(packetReceived);
}

// Mock endpoint that fails reads with a non-critical error
class MockErrorEndpoint : public EndpointInput,
                          public EndpointOutput,
                          public std::enable_shared_from_this<MockErrorEndpoint> {
public:
  int ErrorCount = 0;
  int SuccessCount = 0;

  void AsyncStart(std::move_only_function<Event>&& handler) override { handler(ErrorCode()); }

  void AsyncRead(std::move_only_function<ReadHandler>&& handler) override {
    if (ErrorCount == 0) {
      ++ErrorCount;
      // Simulate a non-critical error
      auto a = std::make_shared<std::array<uint8_t, 1>>();
      auto p = Packet{Buffer(*a), a};
      handler(ErrorCode{AppErrorCategory::kInvalidPacketSession, kAppError}, std::move(p));
    } else if (SuccessCount == 0) {
      ++SuccessCount;
      // Simulate success
      auto a = std::make_shared<std::array<uint8_t, 2048>>();
      auto p = Packet{Buffer(*a), a};
      handler(ErrorCode(), std::move(p));
    }
  }

  void AsyncWrite(Packet&& p, std::move_only_function<WriteHandler>&& handler) override {
    handler(ErrorCode(), p.first.Length);
  }
};

TEST(PipelineTest, RecoversFromNonCriticalReadError) {
  auto mockEp = std::make_shared<MockErrorEndpoint>();

  auto pipe = std::make_shared<Pipeline>(mockEp, std::vector<std::shared_ptr<Filter>>{}, mockEp);
  pipe->Start();

  // When pipeline reads, it should first get an error, then a success, and continue.
  // If it doesn't stop on the error, success_count will become 1.
  EXPECT_EQ(mockEp->ErrorCount, 1);
  EXPECT_EQ(mockEp->SuccessCount, 1);
}
