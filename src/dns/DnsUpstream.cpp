#include "DnsUpstream.hpp"

#include <boost/log/trivial.hpp>
#include <cstddef>

#include "Event.hpp"

namespace gh::dns {

DnsUpstream::DnsUpstream(const boost::asio::any_io_executor& executor,
                         std::vector<boost::asio::ip::udp::endpoint> upstreamServers)
    : _Socket(executor), _UpstreamServers(std::move(upstreamServers)) {}

DnsUpstream::~DnsUpstream() = default;

auto DnsUpstream::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> {
  boost::system::error_code err;
  _Socket.open(boost::asio::ip::udp::v4(), err);
  if (err) {
    co_return err;
  }

  _Socket.bind(boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0), err);
  if (err) {
    _Socket.close();
    co_return err;
  }

  _LocalPort = _Socket.local_endpoint().port();
  co_return ErrorCode{};
}

auto DnsUpstream::DoWork() -> Omni::Fiber::Coroutine<void> { co_await ReceiveLoop(); }

auto DnsUpstream::DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> {
  BOOST_LOG_TRIVIAL(info) << GetName() << " DoGracefulStop start";
  boost::system::error_code err;
  _Socket.close(err);

  for (auto& [txId, pending] : _PendingQueries) {
    if (pending.Event) {
      pending.Event->Fire(std::unexpected(SysError(ECANCELED)));
    }
  }
  _PendingQueries.clear();

  BOOST_LOG_TRIVIAL(info) << GetName() << " DoGracefulStop done";
  co_return ErrorCode{};
}

auto DnsUpstream::GenerateTxId() -> uint16_t {
  uint16_t txId = _NextTxId++;
  if (_NextTxId == 0) {
    _NextTxId = 1;
  }
  return txId;
}

auto DnsUpstream::ReceiveLoop() -> Omni::Fiber::Coroutine<void> {
  constexpr size_t kRxBufferSize = 2048;
  std::vector<uint8_t> rxBuffer(kRxBufferSize);
  boost::asio::ip::udp::endpoint senderEp;

  while (_Service.has_value() && !_Service.value()._Stop.IsTriggered()) {
    auto [err, bytesRecv] = co_await _Socket.async_receive_from(boost::asio::buffer(rxBuffer), senderEp,
                                                                _Service.value()._Stop.AsioSlot()());

    if (err) {
      BOOST_LOG_TRIVIAL(info) << GetName() << " ReceiveLoop rx error: " << err.message();
      if (_Service.value()._Stop.IsTriggered() || !_Socket.is_open()) {
        break;
      }
      continue;
    }

    auto parseResult = DnsPacket::Parse(std::span<const uint8_t>(rxBuffer.data(), bytesRecv));
    if (!parseResult) {
      BOOST_LOG_TRIVIAL(error) << "DnsUpstream rx parse failed";
      continue;
    }

    uint16_t txId = parseResult->Header.Id;
    BOOST_LOG_TRIVIAL(info) << "DnsUpstream rx packet txId=" << txId;
    std::shared_ptr<Omni::Fiber::Event<std::expected<DnsPacket, ErrorCode>>> eventToNotify;
    uint16_t origTxId = 0;

    if (auto matchIt = _PendingQueries.find(txId); matchIt != _PendingQueries.end()) {
      eventToNotify = matchIt->second.Event;
      origTxId = matchIt->second.OriginalTxId;
      _PendingQueries.erase(matchIt);
    } else {
      BOOST_LOG_TRIVIAL(error) << "DnsUpstream txId=" << txId << " not found in pending queries";
    }

    if (eventToNotify) {
      parseResult->Header.Id = origTxId;
      eventToNotify->Fire(std::move(*parseResult));
    }
  }
}

auto DnsUpstream::Resolve(DnsPacket request, Cancel& cancel)
    -> Omni::Fiber::Coroutine<std::expected<DnsPacket, ErrorCode>> {
  if (_UpstreamServers.empty() || GetState() != State::kRunning) {
    BOOST_LOG_TRIVIAL(error) << "DnsUpstream::Resolve rejected: upstreams empty or not running state="
                             << static_cast<int>(GetState());
    co_return std::unexpected(SysError(ENETUNREACH));
  }
  std::vector<boost::asio::ip::udp::endpoint> targets = _UpstreamServers;

  uint16_t origTxId = request.Header.Id;
  uint16_t clientTxId = GenerateTxId();
  auto event = std::make_shared<Omni::Fiber::Event<std::expected<DnsPacket, ErrorCode>>>();

  _PendingQueries[clientTxId] = PendingQuery{.OriginalTxId = origTxId, .Event = event};

  request.Header.Id = clientTxId;
  auto wireBuf = request.Serialize();

  BOOST_LOG_TRIVIAL(info) << "DnsUpstream sending query txId=" << clientTxId << " to " << targets.size()
                          << " upstreams";

  bool sent = false;
  for (const auto& target : targets) {
    auto [err, bytesSent] = co_await _Socket.async_send_to(boost::asio::buffer(wireBuf), target, cancel.AsioSlot()());
    if (!err) {
      sent = true;
      BOOST_LOG_TRIVIAL(info) << "DnsUpstream sent " << bytesSent << " bytes to " << target;
      break;
    } else {
      BOOST_LOG_TRIVIAL(error) << "DnsUpstream async_send_to failed: " << err.message();
    }
  }

  if (!sent) {
    _PendingQueries.erase(clientTxId);
    co_return std::unexpected(SysError(EHOSTUNREACH));
  }

  BOOST_LOG_TRIVIAL(info) << "DnsUpstream awaiting event for txId=" << clientTxId;
  auto res = co_await *event;
  BOOST_LOG_TRIVIAL(info) << "DnsUpstream event received for txId=" << clientTxId;

  _PendingQueries.erase(clientTxId);

  co_return res;
}

} // namespace gh::dns
