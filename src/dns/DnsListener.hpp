#pragma once

#include <string>
#include <vector>

#include <boost/asio.hpp>

#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "ServiceBase.hpp"

namespace gh::dns {

class DnsRouter;

class DnsListener : public ServiceBase {
public:
  explicit DnsListener(boost::asio::any_io_executor executor, boost::asio::ip::udp::endpoint endpoint,
                       DnsRouter& router);
  ~DnsListener() override;

  DnsListener(const DnsListener&) = delete;
  auto operator=(const DnsListener&) -> DnsListener& = delete;
  DnsListener(DnsListener&&) = delete;
  auto operator=(DnsListener&&) -> DnsListener& = delete;

  [[nodiscard]] auto GetName() const -> std::string override;
  [[nodiscard]] auto GetLocalEndpoint() const -> const boost::asio::ip::udp::endpoint& { return _LocalEndpoint; }

  auto SendResponse(boost::asio::ip::udp::endpoint sender, std::vector<uint8_t> data) -> Omni::Fiber::Coroutine<void>;

protected:
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoWork() -> Omni::Fiber::Coroutine<void> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

private:
  boost::asio::any_io_executor _Executor;
  boost::asio::ip::udp::endpoint _LocalEndpoint;
  boost::asio::ip::udp::socket _Socket;
  DnsRouter& _Router;
};

} // namespace gh::dns
