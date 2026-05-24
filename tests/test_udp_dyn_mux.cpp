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
using namespace gh;

TEST(UdpDynMuxTest, SuccessfulNegotiationAndDataTransfer) {
  boost::asio::io_context ioContext;

  // Start raw UDP socket to act as the server
  udp::socket serverSocket(ioContext, udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  udp::endpoint serverEp = serverSocket.local_endpoint();

  // Start client connecting to our mock server
  auto client =
      std::make_shared<UdpDynMuxClient>(ioContext, serverEp, udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));

  bool clientStarted = false;
  client->AsyncStart([&](const ErrorCode& ec) {
    ASSERT_FALSE(ec);
    clientStarted = true;
  });

  // Server: listen for negotiation request
  uint8_t rxBuf[2048];
  udp::endpoint clientEp;
  bool negotiationRequestReceived = false;
  uint32_t clientCookie = 0;

  serverSocket.async_receive_from(boost::asio::buffer(rxBuf), clientEp,
                                  [&](const boost::system::error_code& ec, std::size_t n) {
                                    ASSERT_FALSE(ec);
                                    auto req = UdpDynMux::ClientReqId::Deserialize(rxBuf, n);
                                    ASSERT_TRUE(req.has_value());
                                    clientCookie = req->Cookie;
                                    negotiationRequestReceived = true;

                                    // Send back assignment
                                    uint8_t txBuf[UdpDynMux::ServerAssignId::kSize];
                                    UdpDynMux::ServerAssignId{clientCookie, 1}.Serialize(txBuf);
                                    serverSocket.send_to(boost::asio::buffer(txBuf), clientEp);
                                  });

  // Run until negotiation is complete
  ioContext.restart();
  ioContext.run_for(std::chrono::milliseconds(200));
  ASSERT_TRUE(clientStarted);
  ASSERT_TRUE(negotiationRequestReceived);

  // Send packet client -> server
  bool serverReceived = false;
  serverSocket.async_receive_from(boost::asio::buffer(rxBuf), clientEp,
                                  [&](const boost::system::error_code& ec, std::size_t n) {
                                    ASSERT_FALSE(ec);
                                    ASSERT_GE(n, 2);
                                    uint16_t id = UdpDynMux::ReadUint16Be(rxBuf);
                                    ASSERT_EQ(id, 1);
                                    ASSERT_EQ(std::string((char*)rxBuf + 2, n - 2), "Hello");
                                    serverReceived = true;
                                  });

  auto a = std::make_shared<std::array<uint8_t, 2048>>();
  auto p = Packet{Buffer(*a), a};
  std::memcpy(p.first.Data + p.first.Offset, "Hello", 5);
  p.first.Length = 5;

  client->AsyncWrite(std::move(p), [&](const ErrorCode& ec, std::size_t n) {
    ASSERT_FALSE(ec);
    ASSERT_EQ(n, 7); // 5 bytes payload + 2 bytes header
  });

  ioContext.restart();
  ioContext.run_for(std::chrono::milliseconds(200));
  EXPECT_TRUE(serverReceived);

  // Send packet server -> client
  bool clientReceived = false;
  client->AsyncRead([&](const ErrorCode& ec, Packet&& p) {
    ASSERT_FALSE(ec);
    clientReceived = true;
    ASSERT_EQ(p.first.Length, 5);
    ASSERT_EQ(std::string((char*)p.first.Data + p.first.Offset, p.first.Length), "World");
  });

  uint8_t dataPkg[7];
  UdpDynMux::WriteUint16Be(dataPkg, 1);
  std::memcpy(dataPkg + 2, "World", 5);
  serverSocket.send_to(boost::asio::buffer(dataPkg), clientEp);

  ioContext.restart();
  ioContext.run_for(std::chrono::milliseconds(200));
  EXPECT_TRUE(clientReceived);
}

TEST(UdpDynMuxTest, PacketQueueingDuringNegotiation) {
  boost::asio::io_context ioContext;

  udp::socket serverSocket(ioContext, udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  udp::endpoint serverEp = serverSocket.local_endpoint();

  auto client =
      std::make_shared<UdpDynMuxClient>(ioContext, serverEp, udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));

  // Write packet BEFORE client async_start (so it's queued)
  auto a = std::make_shared<std::array<uint8_t, 2048>>();
  auto p = Packet{Buffer(*a), a};
  std::memcpy(p.first.Data + p.first.Offset, "Queued", 6);
  p.first.Length = 6;

  bool writeCompleted = false;
  client->AsyncWrite(std::move(p), [&](const ErrorCode& ec, std::size_t n) {
    ASSERT_FALSE(ec);
    writeCompleted = true;
  });

  // Start client to trigger negotiation
  client->AsyncStart([&](const ErrorCode&) {});

  // Server handles negotiation and then receives the queued packet
  uint8_t rxBuf[2048];
  udp::endpoint clientEp;
  bool negotiationHandled = false;
  bool packetReceived = false;

  std::function<void()> handleServer = [&]() {
    serverSocket.async_receive_from(boost::asio::buffer(rxBuf), clientEp,
                                    [&](const boost::system::error_code& ec, std::size_t n) {
                                      ASSERT_FALSE(ec);
                                      if (!negotiationHandled) {
                                        auto req = UdpDynMux::ClientReqId::Deserialize(rxBuf, n);
                                        ASSERT_TRUE(req.has_value());
                                        uint8_t txBuf[UdpDynMux::ServerAssignId::kSize];
                                        UdpDynMux::ServerAssignId{req->Cookie, 1}.Serialize(txBuf);
                                        serverSocket.send_to(boost::asio::buffer(txBuf), clientEp);
                                        negotiationHandled = true;
                                        // Look for the queued data packet next
                                        handleServer();
                                      } else {
                                        ASSERT_GE(n, 2);
                                        uint16_t id = UdpDynMux::ReadUint16Be(rxBuf);
                                        ASSERT_EQ(id, 1);
                                        ASSERT_EQ(std::string((char*)rxBuf + 2, n - 2), "Queued");
                                        packetReceived = true;
                                      }
                                    });
  };
  handleServer();

  ioContext.restart();
  ioContext.run_for(std::chrono::milliseconds(400));

  EXPECT_TRUE(writeCompleted);
  EXPECT_TRUE(packetReceived);
}

TEST(UdpDynMuxTest, ServerHandlesMigration) {
  boost::asio::io_context ioContext;

  auto server = std::make_shared<UdpDynMuxServer>(ioContext, udp::endpoint(boost::asio::ip::address_v6::loopback(), 0));
  bool serverStarted = false;
  server->AsyncStart([&](const ErrorCode& ec) {
    ASSERT_FALSE(ec);
    serverStarted = true;
  });

  ioContext.restart();
  ioContext.run_for(std::chrono::milliseconds(100));
  ASSERT_TRUE(serverStarted);

  udp::endpoint serverEp = server->LocalEndpoint();

  // Mock client socket 1
  udp::socket client1(ioContext, udp::endpoint(udp::v6(), 0));
  uint32_t cookie = 0x12345678;

  uint8_t txBuf[2048];
  UdpDynMux::ClientReqId{cookie}.Serialize(txBuf);
  client1.send_to(boost::asio::buffer(txBuf, UdpDynMux::ClientReqId::kSize), serverEp);

  ioContext.restart();
  ioContext.run_for(std::chrono::milliseconds(100));

  // Receive assignment
  uint8_t rxBuf[2048];
  udp::endpoint senderEp;
  size_t n = client1.receive_from(boost::asio::buffer(rxBuf), senderEp);
  auto assign = UdpDynMux::ServerAssignId::Deserialize(rxBuf, n);
  ASSERT_TRUE(assign.has_value());
  ASSERT_EQ(assign->Cookie, cookie);
  uint16_t id = assign->AssignedId;

  // Now "migrate" to client socket 2
  udp::socket client2(ioContext, udp::endpoint(udp::v6(), 0));
  UdpDynMux::ClientAddrMigrate{id, cookie}.Serialize(txBuf);
  client2.send_to(boost::asio::buffer(txBuf, UdpDynMux::ClientAddrMigrate::kSize), serverEp);

  ioContext.restart();
  ioContext.run_for(std::chrono::milliseconds(100));

  // Receive migration ACK on client 2
  n = client2.receive_from(boost::asio::buffer(rxBuf), senderEp);
  auto migrateAck = UdpDynMux::ServerMigrateAck::Deserialize(rxBuf, n);
  ASSERT_TRUE(migrateAck.has_value());
  ASSERT_EQ(migrateAck->Id, id);
}
