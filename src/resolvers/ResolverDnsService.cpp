#include "ResolverDnsService.hpp"

#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <netdb.h>
#include <resolv.h>

#include <cstring>
#include <random>
#include <thread>

#include <boost/log/trivial.hpp>

#include "Asio.hpp"
#include "Cancel.hpp"
#include "Select.hpp"

namespace gh {

struct SRVResult {
  std::string target;
  uint16_t port;
};

ResolverDnsService::ResolverDnsService(const std::string& serviceName, ResolveFor& target)
    : _ServiceName(serviceName), _Target(target) {}

boost::asio::ip::udp::endpoint ResolverDnsService::GetResolverResult() const {
  if (_Endpoints.empty()) {
    return boost::asio::ip::udp::endpoint{};
  }
  static std::random_device rd;
  static std::mt19937 gen(rd());
  std::uniform_int_distribution<size_t> dis(0, _Endpoints.size() - 1);
  return _Endpoints[dis(gen)];
}

Omni::Fiber::Coroutine<ErrorCode> ResolverDnsService::DoStart() {
  if (_Service.value()._Stop.IsTriggered()) {
    co_return make_error_code(boost::asio::error::operation_aborted);
  }
  auto eventPtr = std::make_shared<Omni::Fiber::Event<std::pair<ErrorCode, std::vector<SRVResult>>>>();
  auto workGuard = boost::asio::make_work_guard(_Target.GetExecutor());

  // Run the blocking res_nsearch query in a separate thread.
  std::thread([this, eventPtr, workGuard]() mutable {
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

    boost::asio::post(_Target.GetExecutor(), [eventPtr, workGuard, queryErr, srvResults]() mutable {
      eventPtr->Fire(std::make_pair(queryErr, std::move(srvResults)));
    });
  }).detach();

  bool cancelled = false;
  std::pair<ErrorCode, std::vector<SRVResult>> srvResponse;
  co_await Omni::Fiber::Select(
      Omni::Fiber::SelectPair(_Service.value()._Stop.GetFiberCancelEvent(), [&]() { cancelled = true; }),
      Omni::Fiber::SelectPair(*eventPtr, [&](auto const& response) { srvResponse = response; }));

  if (cancelled) {
    co_return make_error_code(boost::asio::error::operation_aborted);
  }
  if (srvResponse.first) {
    co_return srvResponse.first;
  }

  for (auto const& record : srvResponse.second) {
    if (_Service.value()._Stop.IsTriggered()) {
      co_return make_error_code(boost::asio::error::operation_aborted);
    }
    boost::asio::ip::udp::resolver resolver(_Target.GetExecutor());
    auto [resolveErr, results] = co_await resolver.async_resolve(
        record.target, "",
        boost::asio::bind_cancellation_slot(_Service.value()._Stop.AsioSlot().Slot(), Omni::Fiber::AsioUseFiber));
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
