#include "DnsUpstream.hpp"

#include <cstddef>
#include <exception>
#include <system_error>

#include <boost/log/trivial.hpp>
#include <boost/system/system_error.hpp>

#include "Event.hpp"

namespace gh::dns {

DnsUpstream::DnsUpstream(const boost::asio::any_io_executor& executor, std::string name,
                         std::vector<boost::asio::ip::udp::endpoint> upstreamServers)
    : _Name(std::move(name)), _Socket(executor), _UpstreamServers(std::move(upstreamServers)) {}

DnsUpstream::~DnsUpstream() = default;

auto DnsUpstream::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> {
  try {
    _Socket.open(boost::asio::ip::udp::v4());
    _Socket.bind(boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0));
  } catch (const boost::system::system_error& e) {
    BOOST_LOG_TRIVIAL(info) << GetName() << " start failed: " << e.what();
    if (_Socket.is_open()) {
      boost::system::error_code ignoreEc;
      _Socket.close(ignoreEc);
    }
    co_return e.code();
  } catch (const std::system_error& e) {
    BOOST_LOG_TRIVIAL(info) << GetName() << " start failed: " << e.what();
    if (_Socket.is_open()) {
      boost::system::error_code ignoreEc;
      _Socket.close(ignoreEc);
    }
    co_return e.code();
  } catch (const std::exception& e) {
    BOOST_LOG_TRIVIAL(info) << GetName() << " start failed: " << e.what();
    if (_Socket.is_open()) {
      boost::system::error_code ignoreEc;
      _Socket.close(ignoreEc);
    }
    co_return std::make_error_code(std::errc::io_error);
  }

  _LocalPort = _Socket.local_endpoint().port();
  co_return ErrorCode{};
}

auto DnsUpstream::DoWork() -> Omni::Fiber::Coroutine<void> { co_await ReceiveLoop(); }

auto DnsUpstream::DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> {
  BOOST_LOG_TRIVIAL(info) << GetName() << " DoGracefulStop start";
  _Socket.close();

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
      BOOST_LOG_TRIVIAL(error) << GetName() << " ReceiveLoop rx error: " << err.message();
      if (_Service.value()._Stop.IsTriggered() || !_Socket.is_open()) {
        break;
      }
      continue;
    }

    auto parseResult = DnsPacket::Parse(std::span<const uint8_t>(rxBuffer.data(), bytesRecv));
    if (!parseResult) {
      BOOST_LOG_TRIVIAL(error) << GetName() << " rx parse failed";
      continue;
    }

    uint16_t txId = parseResult->Header.Id;
    BOOST_LOG_TRIVIAL(debug) << GetName() << " rx packet txId=" << txId;
    std::shared_ptr<Omni::Fiber::Event<std::expected<DnsPacket, ErrorCode>>> eventToNotify;
    uint16_t origTxId = 0;

    if (auto matchIt = _PendingQueries.find(txId); matchIt != _PendingQueries.end()) {
      eventToNotify = matchIt->second.Event;
      origTxId = matchIt->second.OriginalTxId;
      _PendingQueries.erase(matchIt);
    } else {
      BOOST_LOG_TRIVIAL(error) << GetName() << " txId=" << txId << " not found in pending queries";
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
    BOOST_LOG_TRIVIAL(error) << GetName() << "::Resolve rejected: upstreams empty or not running state="
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

  BOOST_LOG_TRIVIAL(debug) << GetName() << " sending query txId=" << clientTxId << " to " << targets.size()
                           << " upstreams";

  bool sent = false;
  for (const auto& target : targets) {
    auto [err, bytesSent] = co_await _Socket.async_send_to(boost::asio::buffer(wireBuf), target, cancel.AsioSlot()());
    if (!err) {
      sent = true;
      BOOST_LOG_TRIVIAL(debug) << GetName() << " sent " << bytesSent << " bytes to " << target;
      break;
    } else {
      BOOST_LOG_TRIVIAL(error) << GetName() << " async_send_to failed: " << err.message();
    }
  }

  if (!sent) {
    _PendingQueries.erase(clientTxId);
    co_return std::unexpected(SysError(EHOSTUNREACH));
  }

  BOOST_LOG_TRIVIAL(debug) << GetName() << " awaiting event for txId=" << clientTxId;
  auto res = co_await *event;
  BOOST_LOG_TRIVIAL(debug) << GetName() << " event received for txId=" << clientTxId;

  _PendingQueries.erase(clientTxId);

  co_return res;
}

} // namespace gh::dns
