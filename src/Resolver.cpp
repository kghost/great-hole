#include "Resolver.hpp"

#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <resolv.h>

#include <boost/lexical_cast.hpp>
#include <boost/log/trivial.hpp>
#include <cstring>
#include <random>
#include <thread>

#include "Asio.hpp"
#include "Cancel.hpp"
#include "GetCurrentFiber.hpp"
#include "Select.hpp"

namespace gh {

// ==================== ResolverStaticIp ====================
ResolverStaticIp::ResolverStaticIp(std::string const& ipStr) : _IpStr(ipStr) {}

boost::asio::ip::address ResolverStaticIp::GetAddress() const {
  if (_Addresses.empty()) {
    return boost::asio::ip::address{};
  }
  return _Addresses[0];
}

Omni::Fiber::Coroutine<ErrorCode> ResolverStaticIp::DoStart() {
  try {
    auto addr = boost::asio::ip::make_address(_IpStr);
    _Addresses = {addr};
    co_return ErrorCode{};
  } catch (const boost::system::system_error& e) {
    co_return e.code();
  }
}

Omni::Fiber::Coroutine<ErrorCode> ResolverStaticIp::DoGracefulStop() { co_return ErrorCode{}; }

// ==================== ResolverStaticDns ====================
ResolverStaticDns::ResolverStaticDns(boost::asio::io_context& ioContext, std::string const& host)
    : _IoContext(ioContext), _Host(host) {}

boost::asio::ip::address ResolverStaticDns::GetAddress() const {
  if (_Addresses.empty()) {
    return boost::asio::ip::address{};
  }
  static std::random_device rd;
  static std::mt19937 gen(rd());
  std::uniform_int_distribution<size_t> dis(0, _Addresses.size() - 1);
  return _Addresses[dis(gen)];
}

Omni::Fiber::Coroutine<ErrorCode> ResolverStaticDns::DoStart() {
  if (_Stop.IsTriggered()) {
    co_return make_error_code(boost::asio::error::operation_aborted);
  }
  boost::asio::ip::udp::resolver resolver(_IoContext);
  auto [err, results] = co_await resolver.async_resolve(
      _Host, "", boost::asio::bind_cancellation_slot(_Stop.AsioSlot().Slot(), Omni::Fiber::AsioUseFiber));
  if (err) {
    co_return err;
  }
  for (auto const& entry : results) {
    _Addresses.push_back(entry.endpoint().address());
  }
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> ResolverStaticDns::DoGracefulStop() { co_return ErrorCode{}; }

// ==================== ResolverStaticPort ====================
ResolverStaticPort::ResolverStaticPort(std::string const& portStr) : _PortStr(portStr), _IsNumeric(false) {}
ResolverStaticPort::ResolverStaticPort(uint16_t port) : _Port(port), _IsNumeric(true) {}

uint16_t ResolverStaticPort::GetPort() const { return _Port; }

Omni::Fiber::Coroutine<ErrorCode> ResolverStaticPort::DoStart() {
  if (_IsNumeric) {
    co_return ErrorCode{};
  }
  try {
    int val = boost::lexical_cast<int>(_PortStr);
    if (val < 0 || val > 65535) {
      co_return make_error_code(boost::asio::error::invalid_argument);
    }
    _Port = static_cast<uint16_t>(val);
    co_return ErrorCode{};
  } catch (const boost::bad_lexical_cast&) {
    co_return make_error_code(boost::asio::error::invalid_argument);
  }
}

Omni::Fiber::Coroutine<ErrorCode> ResolverStaticPort::DoGracefulStop() { co_return ErrorCode{}; }

// ==================== ResolverCombinedEndpoint ====================
ResolverCombinedEndpoint::ResolverCombinedEndpoint(boost::asio::io_context& ioContext,
                                                   std::shared_ptr<ResolverIp> ipResolver,
                                                   std::shared_ptr<ResolverPort> portResolver)
    : _IoContext(ioContext), _IpResolver(ipResolver), _PortResolver(portResolver) {}

boost::asio::ip::udp::endpoint ResolverCombinedEndpoint::GetEndpoint() const { return _Endpoint; }

Omni::Fiber::Coroutine<ErrorCode> ResolverCombinedEndpoint::DoStart() {
  auto errIp = co_await _IpResolver->Start();
  if (errIp) {
    co_return errIp;
  }
  auto errPort = co_await _PortResolver->Start();
  if (errPort) {
    co_return errPort;
  }

  auto addr = _IpResolver->GetAddress();
  auto port = _PortResolver->GetPort();
  _Endpoint = boost::asio::ip::udp::endpoint(addr, port);
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> ResolverCombinedEndpoint::DoGracefulStop() {
  co_await _IpResolver->Stop();
  co_await _PortResolver->Stop();
  auto& current = co_await Omni::Fiber::GetCurrentFiber();
  co_await current.WaitAll();
  co_return ErrorCode{};
}

// ==================== ResolverDnsService ====================
struct SRVResult {
  std::string target;
  uint16_t port;
};

ResolverDnsService::ResolverDnsService(boost::asio::io_context& ioContext, std::string const& serviceName)
    : _IoContext(ioContext), _ServiceName(serviceName) {}

boost::asio::ip::udp::endpoint ResolverDnsService::GetEndpoint() const {
  if (_Endpoints.empty()) {
    return boost::asio::ip::udp::endpoint{};
  }
  static std::random_device rd;
  static std::mt19937 gen(rd());
  std::uniform_int_distribution<size_t> dis(0, _Endpoints.size() - 1);
  return _Endpoints[dis(gen)];
}

Omni::Fiber::Coroutine<ErrorCode> ResolverDnsService::DoStart() {
  if (_Stop.IsTriggered()) {
    co_return make_error_code(boost::asio::error::operation_aborted);
  }
  auto eventPtr = std::make_shared<Omni::Fiber::Event<std::pair<ErrorCode, std::vector<SRVResult>>>>();
  auto workGuard = boost::asio::make_work_guard(_IoContext);

  // Run the blocking res_nsearch query in a separate thread.
  std::thread([this, eventPtr, workGuard, &ioContext = _IoContext]() mutable {
    std::vector<SRVResult> srvResults;
    struct __res_state res;
    std::memset(&res, 0, sizeof(res));
    res_ninit(&res);
    unsigned char answer[65536];
    int len = res_nsearch(&res, _ServiceName.c_str(), C_IN, T_SRV, answer, sizeof(answer));
    ErrorCode queryErr;
    if (len < 0) {
      queryErr = make_error_code(boost::asio::error::host_not_found);
    } else {
      ns_msg handle;
      if (ns_initparse(answer, len, &handle) < 0) {
        queryErr = make_error_code(boost::asio::error::invalid_argument);
      } else {
        int count = ns_msg_count(handle, ns_s_an);
        for (int i = 0; i < count; i++) {
          ns_rr rr;
          if (ns_parserr(&handle, ns_s_an, i, &rr) < 0) {
            continue;
          }
          if (ns_rr_type(rr) == T_SRV) {
            const unsigned char* rdata = ns_rr_rdata(rr);
            uint16_t port = ns_get16(rdata + 4);
            char targetBuf[NS_MAXDNAME];
            if (dn_expand(ns_msg_base(handle), ns_msg_end(handle), rdata + 6, targetBuf, sizeof(targetBuf)) >= 0) {
              srvResults.push_back({std::string(targetBuf), port});
            }
          }
        }
      }
    }
    res_nclose(&res);

    boost::asio::post(ioContext, [eventPtr, workGuard, queryErr, srvResults]() mutable {
      eventPtr->Fire(std::make_pair(queryErr, std::move(srvResults)));
    });
  }).detach();

  bool cancelled = false;
  std::pair<ErrorCode, std::vector<SRVResult>> srvResponse;
  co_await Omni::Fiber::Select(
      Omni::Fiber::SelectPair(_Stop.GetFiberCancelEvent(), [&]() { cancelled = true; }),
      Omni::Fiber::SelectPair(*eventPtr, [&](auto const& response) { srvResponse = response; }));

  if (cancelled) {
    co_return make_error_code(boost::asio::error::operation_aborted);
  }
  if (srvResponse.first) {
    co_return srvResponse.first;
  }

  for (auto const& record : srvResponse.second) {
    if (_Stop.IsTriggered()) {
      co_return make_error_code(boost::asio::error::operation_aborted);
    }
    boost::asio::ip::udp::resolver resolver(_IoContext);
    auto [resolveErr, results] = co_await resolver.async_resolve(
        record.target, "", boost::asio::bind_cancellation_slot(_Stop.AsioSlot().Slot(), Omni::Fiber::AsioUseFiber));
    if (resolveErr) {
      BOOST_LOG_TRIVIAL(warning) << "Failed to resolve SRV target: " << record.target << " " << resolveErr.message();
      continue;
    }
    for (auto const& entry : results) {
      _Endpoints.emplace_back(entry.endpoint().address(), record.port);
    }
  }

  if (_Endpoints.empty()) {
    co_return make_error_code(boost::asio::error::host_not_found);
  }

  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> ResolverDnsService::DoGracefulStop() { co_return ErrorCode{}; }

} // namespace gh
