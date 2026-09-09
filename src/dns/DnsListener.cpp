#include "DnsListener.hpp"

#include <utility>

#include <boost/log/trivial.hpp>

#include "Asio.hpp"
#include "DnsRouter.hpp"

namespace gh::dns {

DnsListener::DnsListener(boost::asio::any_io_executor executor, boost::asio::ip::udp::endpoint endpoint,
                         DnsRouter& router)
    : _Executor(std::move(executor)), _LocalEndpoint(std::move(endpoint)), _Socket(_Executor), _Router(router) {}

DnsListener::~DnsListener() = default;

auto DnsListener::GetName() const -> std::string {
  return "DnsListener:" + _LocalEndpoint.address().to_string() + ":" + std::to_string(_LocalEndpoint.port());
}

auto DnsListener::SendResponse(boost::asio::ip::udp::endpoint sender, std::vector<uint8_t> data)
    -> Omni::Fiber::Coroutine<void> {
  if (!_Socket.is_open()) {
    co_return;
  }
  auto [ecSend, nSend] = co_await _Socket.async_send_to(boost::asio::buffer(data), sender, Omni::Fiber::AsioUseFiber);
  if (ecSend) {
    BOOST_LOG_TRIVIAL(error) << GetName() << " async_send_to client failed: " << ecSend.message();
  } else {
    BOOST_LOG_TRIVIAL(info) << GetName() << " sent " << nSend << " bytes response to " << sender;
  }
}

auto DnsListener::DoStart() -> Omni::Fiber::Coroutine<ErrorCode> {
  boost::system::error_code err;
  _Socket.open(_LocalEndpoint.protocol(), err);
  if (err) {
    co_return err;
  }

  _Socket.bind(_LocalEndpoint, err);
  if (err) {
    _Socket.close();
    co_return err;
  }

  boost::system::error_code epErr;
  auto localEp = _Socket.local_endpoint(epErr);
  if (!epErr) {
    _LocalEndpoint = localEp;
  }

  co_return ErrorCode{};
}

auto DnsListener::DoWork() -> Omni::Fiber::Coroutine<void> {
  constexpr size_t kRxBufferSize = 2048;
  std::vector<uint8_t> rxBuffer(kRxBufferSize);
  boost::asio::ip::udp::endpoint senderEp;

  while (_Service.has_value() && !_Service.value()._Stop.IsTriggered()) {
    if (!_Socket.is_open()) {
      break;
    }

    auto [err, bytesRecv] = co_await _Socket.async_receive_from(boost::asio::buffer(rxBuffer), senderEp,
                                                                _Service.value()._Stop.AsioSlot()());

    if (err) {
      if (_Service.value()._Stop.IsTriggered() || !_Socket.is_open()) {
        break;
      }
      continue;
    }

    rxBuffer.resize(bytesRecv);
    co_await _Router.HandleRequest(*this, senderEp, std::move(rxBuffer));
  }
}

auto DnsListener::DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> {
  boost::system::error_code err;
  _Socket.close(err);
  co_return ErrorCode{};
}

} // namespace gh::dns
