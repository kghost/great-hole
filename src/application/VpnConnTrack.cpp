#include "VpnConnTrack.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include <boost/asio.hpp>

#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "Endpoint.hpp"
#include "EndpointUdpDynMux.hpp"
#include "ErrorCode.hpp"
#include "EventQueue.hpp"
#include "GetCurrentFiber.hpp"
#include "Packet.hpp"
#include "Select.hpp"
#include "SelectPair.hpp"
#include "ServiceBase.hpp"
#include "Utils.hpp"

namespace gh {

namespace {

std::optional<ConnectionKey> ParseConnectionKey(const Packet& p) {
  if (p.DataSize() < 20) {
    return std::nullopt;
  }
  uint8_t version = (p.Data()[0] >> 4);
  if (version == 4) {
    uint8_t ihl = (p.Data()[0] & 0x0F) * 4;
    if (p.DataSize() < ihl) {
      return std::nullopt;
    }
    uint8_t protocol = p.Data()[9];
    std::array<uint8_t, 4> srcBytes;
    std::copy_n(p.Data().data() + 12, 4, srcBytes.begin());
    std::array<uint8_t, 4> dstBytes;
    std::copy_n(p.Data().data() + 16, 4, dstBytes.begin());
    auto srcAddr = MapToV6(boost::asio::ip::make_address_v4(srcBytes));
    auto dstAddr = MapToV6(boost::asio::ip::make_address_v4(dstBytes));

    uint16_t srcPort = 0;
    uint16_t dstPort = 0;
    if ((protocol == 6 || protocol == 17) && p.DataSize() >= ihl + 4) {
      srcPort = (p.Data()[ihl] << 8) | p.Data()[ihl + 1];
      dstPort = (p.Data()[ihl + 2] << 8) | p.Data()[ihl + 3];
    } else if (protocol == 1 && p.DataSize() >= ihl + 8) {
      uint8_t type = p.Data()[ihl];
      if (type == 8 || type == 0) {
        uint16_t id = (p.Data()[ihl + 4] << 8) | p.Data()[ihl + 5];
        srcPort = id;
        dstPort = id;
      }
    }
    return ConnectionKey{srcAddr, dstAddr, srcPort, dstPort, protocol};
  } else if (version == 6) {
    if (p.DataSize() < 40) {
      return std::nullopt;
    }
    std::array<uint8_t, 16> srcBytes;
    std::copy_n(p.Data().data() + 8, 16, srcBytes.begin());
    std::array<uint8_t, 16> dstBytes;
    std::copy_n(p.Data().data() + 24, 16, dstBytes.begin());
    auto srcAddr = boost::asio::ip::make_address_v6(srcBytes);
    auto dstAddr = boost::asio::ip::make_address_v6(dstBytes);

    uint8_t nextHeader = p.Data()[6];
    size_t offset = 40;
    uint16_t srcPort = 0;
    uint16_t dstPort = 0;
    uint8_t protocol = nextHeader;

    while (true) {
      if (nextHeader == 6 || nextHeader == 17) {
        if (p.DataSize() >= offset + 4) {
          srcPort = (p.Data()[offset] << 8) | p.Data()[offset + 1];
          dstPort = (p.Data()[offset + 2] << 8) | p.Data()[offset + 3];
          protocol = nextHeader;
        }
        break;
      }
      if (nextHeader == 58) {
        if (p.DataSize() >= offset + 8) {
          uint8_t type = p.Data()[offset];
          if (type == 128 || type == 129) {
            uint16_t id = (p.Data()[offset + 4] << 8) | p.Data()[offset + 5];
            srcPort = id;
            dstPort = id;
          }
          protocol = nextHeader;
        }
        break;
      }
      // Extension headers
      if (nextHeader == 0 || nextHeader == 43 || nextHeader == 60) {
        if (p.DataSize() < offset + 2) {
          break;
        }
        uint8_t extLen = p.Data()[offset + 1];
        nextHeader = p.Data()[offset];
        offset += (extLen + 1) * 8;
      } else if (nextHeader == 44) {
        if (p.DataSize() < offset + 8) {
          break;
        }
        nextHeader = p.Data()[offset];
        offset += 8;
      } else {
        protocol = nextHeader;
        break;
      }
    }
    return ConnectionKey{srcAddr, dstAddr, srcPort, dstPort, protocol};
  }
  return std::nullopt;
}

ConnectionKey CreateCanonical(const boost::asio::ip::address_v6& src, const boost::asio::ip::address_v6& dst,
                              uint16_t srcPort, uint16_t dstPort, uint8_t protocol) {
  if (src < dst) {
    return ConnectionKey{src, dst, srcPort, dstPort, protocol};
  } else if (dst < src) {
    return ConnectionKey{dst, src, dstPort, srcPort, protocol};
  } else {
    if (srcPort <= dstPort) {
      return ConnectionKey{src, dst, srcPort, dstPort, protocol};
    } else {
      return ConnectionKey{dst, src, dstPort, srcPort, protocol};
    }
  }
}

} // namespace

class VpnConnTrack::ChannelSideEndpoint : public EndpointSkipStart<Endpoint> {
public:
  ChannelSideEndpoint(VpnConnTrack& parent, std::shared_ptr<UdpDynMux::Channel> channel)
      : _Parent(parent), _Channel(channel) {}
  ~ChannelSideEndpoint() override = default;

  std::string GetName() const override { return std::format("VpnConnTrack:ChannelSide:[{}]", _Channel->GetName()); }

  Omni::Fiber::Coroutine<ErrorCode> Read(Packet& p, Cancel& c) override {
    while (_Queue.IsEmpty()) {
      if (c.IsTriggered()) {
        co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
      }
      auto [cancelFired, queueFired] = co_await Omni::Fiber::Select(
          Omni::Fiber::SelectPair(c.GetFiberCancelEvent(), [] {}), Omni::Fiber::SelectPair(_Queue, [] {}));
      if (cancelFired) {
        co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
      }
    }
    p = _Queue.PopFront();
    co_return ErrorCode{};
  }

  Omni::Fiber::Coroutine<ErrorCode> Write(Packet& p, Cancel& c) override {
    if (c.IsTriggered()) {
      co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
    }
    auto keyOpt = ParseConnectionKey(p);
    if (keyOpt) {
      auto canonicalKey = CreateCanonical(keyOpt->Addr1, keyOpt->Addr2, keyOpt->Port1, keyOpt->Port2, keyOpt->Protocol);
      _Parent.UpdateConntrack(canonicalKey, _Channel);
    }
    _Parent._Queue.Push(std::move(p));
    co_return ErrorCode{};
  }

  void PushOutgoing(Packet p) { _Queue.Push(std::move(p)); }

protected:
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override { co_return ErrorCode{}; }
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override { co_return ErrorCode{}; }

private:
  VpnConnTrack& _Parent;
  std::shared_ptr<UdpDynMux::Channel> _Channel;
  Omni::Fiber::EventQueue<Packet> _Queue;
};

class VpnConnTrack::TunSideEndpoint : public EndpointSkipStart<Endpoint> {
public:
  explicit TunSideEndpoint(VpnConnTrack& parent) : _Parent(parent) {}
  ~TunSideEndpoint() override = default;

  std::string GetName() const override { return "VpnConnTrack:TunSide"; }

  Omni::Fiber::Coroutine<ErrorCode> Read(Packet& p, Cancel& c) override {
    while (_Parent._Queue.IsEmpty()) {
      if (c.IsTriggered()) {
        co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
      }
      auto [cancelFired, queueFired] = co_await Omni::Fiber::Select(
          Omni::Fiber::SelectPair(c.GetFiberCancelEvent(), [] {}), Omni::Fiber::SelectPair(_Parent._Queue, [] {}));
      if (cancelFired) {
        co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
      }
    }

    p = _Parent._Queue.PopFront();
    co_return ErrorCode{};
  }

  Omni::Fiber::Coroutine<ErrorCode> Write(Packet& p, Cancel& c) override {
    if (c.IsTriggered()) {
      co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
    }

    auto keyOpt = ParseConnectionKey(p);
    if (!keyOpt) {
      BOOST_LOG_TRIVIAL(debug) << _Parent.GetName() << ": Dropping invalid outgoing packet";
      co_return ErrorCode{};
    }

    auto canonicalKey = CreateCanonical(keyOpt->Addr1, keyOpt->Addr2, keyOpt->Port1, keyOpt->Port2, keyOpt->Protocol);

    std::shared_ptr<UdpDynMux::Channel> channel;

    auto now = std::chrono::steady_clock::now();
    auto it = _Parent._ConntrackTable.find(canonicalKey);
    if (it != _Parent._ConntrackTable.end()) {
      if (now - it->second.LastActive <= _Parent._ConntrackTimeout &&
          _Parent._EstablishedChannels.contains(it->second.Channel)) {
        it->second.LastActive = now;
        channel = it->second.Channel;
      } else {
        _Parent._ConntrackTable.erase(it);
      }
    }

    if (!channel && _Parent._Selector) {
      auto selectedChannel =
          _Parent._Selector(keyOpt->Addr1, keyOpt->Addr2, keyOpt->Port1, keyOpt->Port2, keyOpt->Protocol);
      if (selectedChannel && _Parent._EstablishedChannels.contains(selectedChannel)) {
        _Parent._ConntrackTable[canonicalKey] = ConnectionEntry{selectedChannel, now};
        channel = selectedChannel;
      }
    }

    if (!channel) {
      BOOST_LOG_TRIVIAL(debug) << _Parent.GetName() << ": No channel found or selected, dropping packet";
      co_return ErrorCode{};
    }

    auto sessionIt = _Parent._Sessions.find(channel);
    if (sessionIt != _Parent._Sessions.end()) {
      sessionIt->second.ChannelSide->PushOutgoing(std::move(p));
    } else {
      BOOST_LOG_TRIVIAL(debug) << _Parent.GetName() << ": Session for channel not found, dropping packet";
    }

    co_return ErrorCode{};
  }

protected:
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override { co_return ErrorCode{}; }
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override { co_return ErrorCode{}; }

private:
  VpnConnTrack& _Parent;
};

VpnConnTrack::VpnConnTrack(boost::asio::io_context& ioContext, std::shared_ptr<Endpoint> tun,
                           std::shared_ptr<UdpDynMux> udpDynMux, PeerSelector selector,
                           std::vector<std::shared_ptr<Filter>> filters)
    : _IoContext(ioContext), _Tun(tun), _UdpDynMux(udpDynMux), _Selector(selector), _Filters(filters) {
  _TunSide = std::make_shared<TunSideEndpoint>(*this);
  if (_UdpDynMux) {
    _UdpDynMux->SetChannelNotification(*this);
  }
}

VpnConnTrack::~VpnConnTrack() {
  assert(_Sessions.empty());
  assert(_EstablishedChannels.empty());
}

std::string VpnConnTrack::GetName() const {
  return std::format("VpnConnTrack:[{}]", _UdpDynMux ? _UdpDynMux->GetName() : "null");
}

Omni::Fiber::Coroutine<ErrorCode> VpnConnTrack::DoStart() {
  _TunPipeline = std::make_shared<Pipeline>(_Tun, std::vector<std::shared_ptr<Filter>>{}, _TunSide);
  auto err = co_await _TunPipeline->Start();
  if (err) {
    BOOST_LOG_TRIVIAL(error) << GetName() << ": Failed to start TUN pipeline";
    co_return err;
  }
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<void> VpnConnTrack::DoWork() {
  BOOST_LOG_TRIVIAL(info) << GetName() << " run loop started";

  auto& currentFiber = co_await Omni::Fiber::GetCurrentFiber();
  auto pruneFiber = currentFiber.Spawn(GetName() + ":PruneLoop", [this]() -> Omni::Fiber::Coroutine<void> {
    co_await PruneLoop();
    co_return;
  });

  bool stopped = false;
  while (!stopped) {
    auto [stopResult, rpcResult] = co_await Omni::Fiber::Select(
        Omni::Fiber::SelectPair(_Service.value()._Stop.GetFiberCancelEvent(), [] {}),
        Omni::Fiber::SelectPair(_ChannelCall.GetServiceAwaitor(), Omni::Fiber::RemoteCall::HandleRequest));
    if (stopResult) {
      stopped = true;
    }
    if (rpcResult.has_value() && !rpcResult.value()) {
      stopped = true;
    }
  }

  co_await currentFiber.Join(pruneFiber);
  BOOST_LOG_TRIVIAL(info) << GetName() << " run loop finished";
}

Omni::Fiber::Coroutine<ErrorCode> VpnConnTrack::DoGracefulStop() {
  BOOST_LOG_TRIVIAL(info) << GetName() << " stopping";

  if (_TunPipeline) {
    co_await _TunPipeline->Stop();
    _TunPipeline.reset();
  }

  for (auto& [channel, session] : _Sessions) {
    co_await session.Pipeline->Stop();
  }
  _Sessions.clear();
  _EstablishedChannels.clear();
  _ConntrackTable.clear();

  co_await _ChannelCall.Close();

  BOOST_LOG_TRIVIAL(info) << GetName() << " stopped";
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<void> VpnConnTrack::OnChannelEstablished(std::shared_ptr<UdpDynMux::Channel> channel) {
  BOOST_LOG_TRIVIAL(info) << GetName() << ": on channel established: " << channel->GetName();
  co_await _ChannelCall.Call([this, channel = std::move(channel)]() mutable -> Omni::Fiber::Coroutine<void> {
    if (_State != State::kRunning) {
      co_return;
    }
    _EstablishedChannels.insert(channel);

    auto channelSide = std::make_shared<ChannelSideEndpoint>(*this, channel);
    auto pipeline = std::make_shared<Pipeline>(channel, _Filters, channelSide);
    auto err = co_await pipeline->Start();
    if (err) {
      BOOST_LOG_TRIVIAL(error) << GetName() << ": Failed to start channel pipeline for " << channel->GetName();
      co_await pipeline->Stop();
      _EstablishedChannels.erase(channel);
      co_return;
    }

    _Sessions.emplace(std::move(channel), Session{std::move(channelSide), std::move(pipeline)});
  });
}

Omni::Fiber::Coroutine<void> VpnConnTrack::OnChannelClosed(std::shared_ptr<UdpDynMux::Channel> channel) {
  BOOST_LOG_TRIVIAL(info) << GetName() << ": on channel closed: " << channel->GetName();
  co_await _ChannelCall.Call([this, channel = std::move(channel)]() mutable -> Omni::Fiber::Coroutine<void> {
    auto it = _Sessions.find(channel);
    if (it != _Sessions.end()) {
      co_await it->second.Pipeline->Stop();
      _Sessions.erase(it);
    }
    _EstablishedChannels.erase(channel);
    std::erase_if(_ConntrackTable, [channel](const auto& item) { return item.second.Channel == channel; });
  });
}

void VpnConnTrack::UpdateConntrack(const ConnectionKey& key, std::shared_ptr<UdpDynMux::Channel> channel) {
  _ConntrackTable[key] = ConnectionEntry{channel, std::chrono::steady_clock::now()};
}

Omni::Fiber::Coroutine<void> VpnConnTrack::PruneLoop() {
  boost::asio::steady_timer pruneTimer(_IoContext.get_executor());
  while (_State == State::kRunning && !_Service.value()._Stop.IsTriggered()) {
    pruneTimer.expires_after(std::chrono::seconds(5));
    auto [err] = co_await pruneTimer.async_wait(_Service.value()._Stop.AsioSlot()());
    if (err) {
      break;
    }

    auto now = std::chrono::steady_clock::now();
    std::erase_if(_ConntrackTable,
                  [this, now](const auto& item) { return now - item.second.LastActive > _ConntrackTimeout; });
  }
}

} // namespace gh
