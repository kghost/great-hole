#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <unordered_map>
#include <vector>

#include <boost/asio.hpp>

#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "DnsPacket.hpp"
#include "ErrorCode.hpp"
#include "Event.hpp"
#include "ServiceBase.hpp"

namespace gh::dns {

class DnsUpstream : public ServiceBase {
public:
  explicit DnsUpstream(const boost::asio::any_io_executor& executor,
                       std::vector<boost::asio::ip::udp::endpoint> upstreamServers);
  ~DnsUpstream() override;

  DnsUpstream(const DnsUpstream&) = delete;
  auto operator=(const DnsUpstream&) -> DnsUpstream& = delete;
  DnsUpstream(DnsUpstream&&) = delete;
  auto operator=(DnsUpstream&&) -> DnsUpstream& = delete;

  [[nodiscard]] auto GetName() const -> std::string override { return "DnsUpstream:" + std::to_string(_LocalPort); }

  [[nodiscard]] auto GetLocalPort() const -> uint16_t { return _LocalPort; }

  auto Resolve(DnsPacket request, Cancel& cancel) -> Omni::Fiber::Coroutine<std::expected<DnsPacket, ErrorCode>>;

protected:
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoWork() -> Omni::Fiber::Coroutine<void> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

private:
  struct PendingQuery {
    uint16_t OriginalTxId{0};
    std::shared_ptr<Omni::Fiber::Event<std::expected<DnsPacket, ErrorCode>>> Event;
  };

  boost::asio::ip::udp::socket _Socket;
  uint16_t _LocalPort{0};

  std::vector<boost::asio::ip::udp::endpoint> _UpstreamServers;
  std::unordered_map<uint16_t, PendingQuery> _PendingQueries;
  uint16_t _NextTxId{1};

  auto ReceiveLoop() -> Omni::Fiber::Coroutine<void>;
  auto GenerateTxId() -> uint16_t;
};

} // namespace gh::dns
