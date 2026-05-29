#include "EndpointUdpDynMuxServer.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <expected>
#include <format>
#include <map>
#include <memory>
#include <ranges>
#include <utility>

#include <boost/asio/buffer.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/log/trivial.hpp>

#include "Asio.hpp"
#include "Cancel.hpp"
#include "EndpointUdpDynMuxProtocol.hpp"
#include "ErrorCode.hpp"
#include "GetCurrentFiber.hpp"
#include "Select.hpp"

namespace gh {

// ==================== UdpDynMuxServer::Channel ====================

UdpDynMuxServer::Channel::Channel(std::shared_ptr<UdpDynMuxServer> parent, uint16_t id) : _Parent(parent), _Id(id) {}

UdpDynMuxServer::Channel::~Channel() { _Parent->RemoveChannel(_Id); }

std::string UdpDynMuxServer::Channel::GetName() const { return std::format("UdpDynMuxChannel:[{}]", _Id); }

Omni::Fiber::Coroutine<ErrorCode> UdpDynMuxServer::Channel::DoStart() { co_return ErrorCode{}; }

Omni::Fiber::Coroutine<ErrorCode> UdpDynMuxServer::Channel::DoGracefulStop() {
  co_await _PipielineUsageCounter.WaitAll();
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> UdpDynMuxServer::Channel::Read(Packet& p, Cancel& c) {
  bool stopped = false;
  std::optional<ErrorCode> err;
  co_await Omni::Fiber::Select(Omni::Fiber::SelectPair(c.GetFiberCancelEvent(), [&]() { stopped = true; }),
                               Omni::Fiber::SelectPair(_Pipe.GetConsumer(), [&](auto data) {
                                 if (data.has_value()) {
                                   auto& inner = data.value();
                                   if (inner.has_value()) {
                                     p = std::move(inner.value());
                                     err = ErrorCode{};
                                   } else {
                                     err = inner.error();
                                   }
                                 } else {
                                   p._Length = 0;
                                   err = ErrorCode{AppErrorCategory::kEndOfStream, kAppError};
                                 }
                               }));
  if (err.has_value()) {
    co_return err.value();
  }
  if (stopped) {
    co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
  }
  assert(false && "should not reach here");
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> UdpDynMuxServer::Channel::Write(Packet& p, Cancel& c) {
  co_return co_await _Parent->WriteTo(_Id, p, c);
}

// ==================== UdpDynMuxServer ====================

UdpDynMuxServer::UdpDynMuxServer(boost::asio::io_context& ioContext)
    : _Socket(ioContext), _Local(boost::asio::ip::udp::v6(), 0) {}

UdpDynMuxServer::UdpDynMuxServer(boost::asio::io_context& ioContext, boost::asio::ip::udp::endpoint bind)
    : _Socket(ioContext), _Local(bind) {}

UdpDynMuxServer::~UdpDynMuxServer() {}

std::string UdpDynMuxServer::GetName() const { return "UdpDynMuxServer:" + boost::lexical_cast<std::string>(_Local); }

Omni::Fiber::Coroutine<ErrorCode> UdpDynMuxServer::DoStart() {
  try {
    _Socket.open(boost::asio::ip::udp::v6());
    _Socket.set_option(boost::asio::ip::v6_only(false));
    _Socket.bind(_Local);
    BOOST_LOG_TRIVIAL(info) << "UdpDynMuxServer(" << this << ") bound at " << _Socket.local_endpoint();
  } catch (const SystemError& e) {
    BOOST_LOG_TRIVIAL(info) << "UdpDynMuxServer(" << this << ") start failed: " << e.what();
    co_return e.code();
  }
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<void> UdpDynMuxServer::DoWork() {
  auto& currentFiber = co_await Omni::Fiber::GetCurrentFiber();
  currentFiber.Spawn("UdpDynMuxServer ReadLoop:" + boost::lexical_cast<std::string>(LocalEndpoint()) + "@" +
                         std::to_string(reinterpret_cast<uintptr_t>(this)),
                     [this]() -> Omni::Fiber::Coroutine<void> {
                       co_await ReadLoop();
                       co_return;
                     });

  currentFiber.Spawn("UdpDynMuxServer KeepaliveLoop:" + boost::lexical_cast<std::string>(LocalEndpoint()) + "@" +
                         std::to_string(reinterpret_cast<uintptr_t>(this)),
                     [this]() -> Omni::Fiber::Coroutine<void> {
                       co_await KeepaliveLoop();
                       co_return;
                     });

  bool stopped = false;
  while (!stopped) {
    std::optional<std::move_only_function<Omni::Fiber::Coroutine<void>()>> pendingFunc;
    co_await Omni::Fiber::Select(Omni::Fiber::SelectPair(_Stop.GetFiberCancelEvent(), [&]() { stopped = true; }),
                                 Omni::Fiber::SelectPair(_CreateChannelPipe.GetConsumer(), [&](auto data) {
                                   if (data.has_value()) {
                                     pendingFunc = std::move(data.value());
                                   } else {
                                     stopped = true;
                                   }
                                 }));
    if (pendingFunc.has_value()) {
      co_await pendingFunc.value()();
    }
  }
}

Omni::Fiber::Coroutine<ErrorCode> UdpDynMuxServer::DoGracefulStop() {
  for (auto& [id, info] : _Channels) {
    if (auto ch = info.WeakChannel.lock()) {
      co_await ch->Stop();
    }
  }
  auto& currentFiber = co_await Omni::Fiber::GetCurrentFiber();
  co_await currentFiber.WaitAll();
  _Socket.close();
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<std::shared_ptr<Endpoint>> UdpDynMuxServer::CreateChannel(uint16_t id) {
  auto ch = std::make_shared<Channel>(std::static_pointer_cast<UdpDynMuxServer>(shared_from_this()), id);
  _Channels[id] = ChannelInfo{.WeakChannel = ch, .Peer = std::nullopt};

  co_await _CreateChannelPipe.GetProducer().Put([this, id, ch]() -> Omni::Fiber::Coroutine<void> {
    auto err = co_await ch->Start();
    if (err) {
      BOOST_LOG_TRIVIAL(error) << "UdpDynMuxServer Channel start failed: " << err.message();
    }
    co_return;
  });

  co_return ch;
}

void UdpDynMuxServer::RemoveChannel(uint16_t id) { _Channels.erase(id); }

Omni::Fiber::Coroutine<void> UdpDynMuxServer::ReadLoop() {
  auto slotTracker = _Stop.AsioSlot();
  while (!_Stop.IsTriggered()) {
    Packet p;
    boost::asio::ip::udp::endpoint peer;

    auto [err, bytes_transferred] = co_await _Socket.async_receive_from(
        boost::asio::mutable_buffer(p), peer,
        boost::asio::bind_cancellation_slot(slotTracker.Slot(), Omni::Fiber::AsioUseFiber));
    if (err) {
      if (err == boost::asio::error::operation_aborted) {
        BOOST_LOG_TRIVIAL(info) << "UdpDynMuxServer(" << this << ") read loop cancelled";
      } else {
        BOOST_LOG_TRIVIAL(error) << "UdpDynMuxServer(" << this << ") read error: " << err.message();
        for (auto& [id, info] : _Channels) {
          if (auto ch = info.WeakChannel.lock()) {
            if (!ch->IsStopped()) {
              co_await ch->Send(std::unexpected(err));
            }
          }
        }
      }
      break;
    }

    if (bytes_transferred < 2) {
      BOOST_LOG_TRIVIAL(info) << "UdpDynMuxServer(" << this << ") ignored empty/short packet";
      continue;
    }

    uint16_t channelId = UdpDynMux::ReadUint16Be(p._Data.data() + p._Offset);
    auto now = std::chrono::steady_clock::now();

    if (channelId == 0) {
      // Control packet
      if (bytes_transferred >= 3) {
        uint8_t msgType = p._Data[p._Offset + 2];
        if (msgType == static_cast<uint8_t>(UdpDynMux::MsgType::kClientReqId)) {
          if (auto req = UdpDynMux::ClientReqId::Deserialize(p._Data.data() + p._Offset, bytes_transferred)) {
            uint32_t cookie = req->Cookie;

            // Check if this endpoint already has a session
            auto it = std::ranges::find_if(_Channels, [&peer](const auto& pair) { return pair.second.Peer == peer; });
            if (it != _Channels.end()) {
              auto& c = it->second;
              c.Cookie = cookie;
              c.LastSeen = now;
              c.MissingAcks = 0;
              BOOST_LOG_TRIVIAL(info) << "UdpDynMuxServer(" << this << ") re-assigning ID: " << it->first
                                      << " to peer: " << peer;
              co_await SendControlAssign(peer, cookie, it->first);
            } else {
              // Find a free channel ID dynamically from our created channels
              uint16_t freeId = 0;
              for (auto& [candidateId, chInfo] : _Channels) {
                if (!chInfo.Peer.has_value() && chInfo.Cookie == 0) {
                  freeId = candidateId;
                  break;
                }
              }

              if (freeId != 0) {
                auto& c = _Channels[freeId];
                c.Peer = peer;
                c.Cookie = cookie;
                c.LastSeen = now;
                c.MissingAcks = 0;
                BOOST_LOG_TRIVIAL(info) << "UdpDynMuxServer(" << this << ") assigned dynamic ID: " << freeId
                                        << " to peer: " << peer;
                co_await SendControlAssign(peer, cookie, freeId);
              } else {
                BOOST_LOG_TRIVIAL(warning)
                    << "UdpDynMuxServer(" << this << ") allocation failed: no free dynamic channels created";
                co_await SendControlAssign(peer, cookie, 0);
              }
            }
          }
        } else if (msgType == static_cast<uint8_t>(UdpDynMux::MsgType::kClientKeepaliveAck)) {
          if (auto ack = UdpDynMux::ClientKeepaliveAck::Deserialize(p._Data.data() + p._Offset, bytes_transferred)) {
            uint16_t id = ack->Id;
            auto peerIt = _Channels.find(id);
            if (peerIt != _Channels.end()) {
              auto& c = peerIt->second;
              if (c.Peer == peer) {
                c.MissingAcks = 0;
                c.LastSeen = now;
              } else {
                co_await SendControlAddrMismatch(peer, id);
              }
            } else {
              co_await SendControlInvalidId(peer, id);
            }
          }
        } else if (msgType == static_cast<uint8_t>(UdpDynMux::MsgType::kClientAddrMigrate)) {
          if (auto migrate = UdpDynMux::ClientAddrMigrate::Deserialize(p._Data.data() + p._Offset, bytes_transferred)) {
            uint16_t id = migrate->Id;
            uint32_t cookie = migrate->Cookie;

            auto peerIt = _Channels.find(id);
            if (peerIt != _Channels.end()) {
              auto& c = peerIt->second;
              if (c.Cookie == cookie) {
                BOOST_LOG_TRIVIAL(info) << "UdpDynMuxServer(" << this << ") migrating ID " << id << " from "
                                        << (c.Peer ? boost::lexical_cast<std::string>(*c.Peer) : "none") << " to "
                                        << peer;
                c.Peer = peer;
                c.LastSeen = now;
                c.MissingAcks = 0;
                co_await SendControlMigrateAck(peer, id);
              } else {
                BOOST_LOG_TRIVIAL(warning) << "UdpDynMuxServer(" << this << ") migration cookie mismatch for ID " << id
                                           << " from " << (c.Peer ? boost::lexical_cast<std::string>(*c.Peer) : "none")
                                           << " to " << peer << "; cookie: " << c.Cookie << " != " << cookie;
                co_await SendControlCookieMismatch(peer, id);
              }
            } else {
              co_await SendControlInvalidId(peer, id);
            }
          }
        }
      }
    } else {
      // Data packet
      auto peerIt = _Channels.find(channelId);
      if (peerIt != _Channels.end()) {
        auto& c = peerIt->second;
        if (c.Peer == peer) {
          c.MissingAcks = 0;
          c.LastSeen = now;
          if (auto ch = c.WeakChannel.lock()) {
            p._Offset += 2;
            p._Length = bytes_transferred - 2;
            co_await ch->Send(std::move(p));
          } else {
            BOOST_LOG_TRIVIAL(info) << "UdpDynMuxServer(" << this << ") packet from lost channel ID: " << channelId;
          }
        } else {
          co_await SendControlAddrMismatch(peer, channelId);
        }
      } else {
        co_await SendControlInvalidId(peer, channelId);
      }
    }
  }
}

Omni::Fiber::Coroutine<void> UdpDynMuxServer::KeepaliveLoop() {
  auto slotTracker = _Stop.AsioSlot();
  boost::asio::steady_timer timer(_Socket.get_executor());
  while (!_Stop.IsTriggered()) {
    timer.expires_after(std::chrono::seconds(5));
    auto [err] =
        co_await timer.async_wait(boost::asio::bind_cancellation_slot(slotTracker.Slot(), Omni::Fiber::AsioUseFiber));
    if (err) {
      break;
    }

    auto now = std::chrono::steady_clock::now();
    for (auto& [id, c] : _Channels) {
      if (c.Peer.has_value()) {
        c.MissingAcks++;
        if (c.MissingAcks >= 3) {
          BOOST_LOG_TRIVIAL(info) << "UdpDynMuxServer(" << this << ") peer timeout for ID: " << id
                                  << ", releasing session";
          auto peer = *c.Peer;
          c.Peer = std::nullopt;
          c.Cookie = 0;
          c.MissingAcks = 0;

          if (auto ch = c.WeakChannel.lock()) {
            co_await ch->Send(std::unexpected(ErrorCode{AppErrorCategory::kEndOfStream, kAppError}));
          }
          co_await SendControlIdClosed(peer, id);
        } else {
          if (now > c.LastSeen + std::chrono::seconds(5)) {
            co_await SendControlKeepalive(*c.Peer, id);
          }
        }
      }
    }
  }
}

Omni::Fiber::Coroutine<ErrorCode> UdpDynMuxServer::WriteTo(uint16_t id, Packet& p, Cancel& c) {
  if (c.IsTriggered()) {
    co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
  }

  auto it = _Channels.find(id);
  if (it == _Channels.end() || !it->second.Peer.has_value()) {
    co_return ErrorCode{AppErrorCategory::kInvalidPacketSession, kAppError};
  }

  if (p._Offset < 2) {
    co_return ErrorCode{AppErrorCategory::kInvalidPacketReserved, kAppError};
  }

  p._Offset -= 2;
  p._Length += 2;
  UdpDynMux::WriteUint16Be(p._Data.data() + p._Offset, id);

  auto [err, bytes_transferred] = co_await _Socket.async_send_to(
      boost::asio::const_buffer(p), it->second.Peer.value(),
      boost::asio::bind_cancellation_slot(c.AsioSlot().Slot(), Omni::Fiber::AsioUseFiber));
  assert(err || bytes_transferred == p._Length);
  co_return err;
}

bool UdpDynMuxServer::CheckRateLimit(const boost::asio::ip::udp::endpoint& peer) {
  auto now = std::chrono::steady_clock::now();
  auto it = _LastErrorSent.find(peer);
  if (it != _LastErrorSent.end() && now - it->second < std::chrono::seconds(1)) {
    return false;
  }
  _LastErrorSent[peer] = now;
  return true;
}

template <typename T>
static Omni::Fiber::Coroutine<void> SendControlPacket(boost::asio::ip::udp::socket& socket,
                                                      const boost::asio::ip::udp::endpoint& peer, const T& msg) {
  auto buf = std::make_shared<std::array<uint8_t, T::kSize>>();
  msg.Serialize(*buf);
  co_await socket.async_send_to(boost::asio::buffer(*buf), peer, Omni::Fiber::AsioUseFiber);
}

Omni::Fiber::Coroutine<void> UdpDynMuxServer::SendControlAssign(const boost::asio::ip::udp::endpoint& peer,
                                                                uint32_t cookie, uint16_t id) {
  co_await SendControlPacket(_Socket, peer, UdpDynMux::ServerAssignId{cookie, id});
}

Omni::Fiber::Coroutine<void> UdpDynMuxServer::SendControlKeepalive(const boost::asio::ip::udp::endpoint& peer,
                                                                   uint16_t id) {
  co_await SendControlPacket(_Socket, peer, UdpDynMux::ServerKeepalive{id});
}

Omni::Fiber::Coroutine<void> UdpDynMuxServer::SendControlIdClosed(const boost::asio::ip::udp::endpoint& peer,
                                                                  uint16_t id) {
  co_await SendControlPacket(_Socket, peer, UdpDynMux::ServerIdClosed{id});
}

Omni::Fiber::Coroutine<void> UdpDynMuxServer::SendControlAddrMismatch(const boost::asio::ip::udp::endpoint& peer,
                                                                      uint16_t id) {
  if (!CheckRateLimit(peer)) {
    co_return;
  }
  co_await SendControlPacket(_Socket, peer, UdpDynMux::ServerAddrMismatch{id});
}

Omni::Fiber::Coroutine<void> UdpDynMuxServer::SendControlMigrateAck(const boost::asio::ip::udp::endpoint& peer,
                                                                    uint16_t id) {
  co_await SendControlPacket(_Socket, peer, UdpDynMux::ServerMigrateAck{id});
}

Omni::Fiber::Coroutine<void> UdpDynMuxServer::SendControlInvalidId(const boost::asio::ip::udp::endpoint& peer,
                                                                   uint16_t id) {
  if (!CheckRateLimit(peer)) {
    co_return;
  }
  co_await SendControlPacket(_Socket, peer, UdpDynMux::ServerInvalidId{id});
}

Omni::Fiber::Coroutine<void> UdpDynMuxServer::SendControlCookieMismatch(const boost::asio::ip::udp::endpoint& peer,
                                                                        uint16_t id) {
  if (!CheckRateLimit(peer)) {
    co_return;
  }
  co_await SendControlPacket(_Socket, peer, UdpDynMux::ServerCookieMismatch{id});
}

} // namespace gh
