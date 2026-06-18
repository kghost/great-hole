#include "AresResolver.hpp"

#include <chrono>
#include <cstring>
#include <expected>
#include <functional>
#include <unordered_map>

#include <ares.h>
#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <netdb.h>

#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/log/trivial.hpp>

#include "Asio.hpp"
#include "Cancel.hpp"
#include "ErrorCode.hpp"
#include "Select.hpp"
#include "SelectPair.hpp"
#include "SelectPairList.hpp"
#include "Utils.hpp"

namespace gh {

namespace {

class AresGlobal {
public:
  explicit AresGlobal();
  ~AresGlobal();
};

AresGlobal::AresGlobal() { (void)ares_library_init(ARES_LIB_INIT_ALL); }
AresGlobal::~AresGlobal() { ares_library_cleanup(); }
AresGlobal Ares;

template <typename Lambda, typename... Args> void LambdaBridge(void* data, Args... args) {
  (*reinterpret_cast<Lambda*>(data))(args...);
}

struct SocketTracker {
  explicit SocketTracker(boost::asio::any_io_executor executor, ares_socket_t fd) : Descriptor(executor, fd) {
    if (SocketProtector) {
      if (!SocketProtector(fd)) {
        BOOST_LOG_TRIVIAL(warning) << "Failed to protect c-ares socket fd: " << fd;
      }
    }
  }
  ~SocketTracker() { Descriptor.release(); }

  SocketTracker(SocketTracker&&) = default;

  boost::asio::posix::stream_descriptor Descriptor;
  bool ReadableInterest = false;
  bool WritableInterest = false;
};

template <typename InitiateQuery>
  requires std::same_as<decltype(std::declval<InitiateQuery>()(std::declval<ares_channel_t*>())), ErrorCode>
Omni::Fiber::Coroutine<ErrorCode> RunChannel(boost::asio::any_io_executor executor, InitiateQuery&& initiateQuery,
                                             Cancel& cancel) {
  if (cancel.IsTriggered()) {
    co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
  }

  std::unordered_map<ares_socket_t, SocketTracker> trackers;

  auto OnSocketStateChange = [&](ares_socket_t fd, int readable, int writable) {
    auto it = trackers.find(fd);
    if (!readable && !writable) {
      if (it != trackers.end()) {
        trackers.erase(it);
      }
      return;
    }

    if (it == trackers.end()) {
      auto [newIt, inserted] = trackers.emplace(fd, SocketTracker(executor, fd));
      it = newIt;
    }

    auto& tracker = it->second;
    tracker.ReadableInterest = readable;
    tracker.WritableInterest = writable;
  };

  struct ares_options options;
  int optmask = 0;
  options.sock_state_cb = LambdaBridge<decltype(OnSocketStateChange), ares_socket_t, int, int>;
  options.sock_state_cb_data = &OnSocketStateChange;
  optmask |= ARES_OPT_SOCK_STATE_CB;

  ares_channel_t* channel = nullptr;
  int status = ares_init_options(&channel, &options, optmask);
  if (status != ARES_SUCCESS) {
    co_return make_error_code(boost::asio::error::no_recovery);
  }

  ErrorCode err = initiateQuery(channel);
  while (!err) {
    Omni::Fiber::SelectPairList<Omni::Fiber::AsioResult<boost::system::error_code>,
                                std::function<void(std::tuple<boost::system::error_code>)>>
        list;

    boost::asio::steady_timer timer(executor);
    struct timeval tv;
    struct timeval* tvPtr = ares_timeout(channel, nullptr, &tv);
    if (tvPtr) {
      auto duration = std::chrono::seconds(tv.tv_sec) + std::chrono::microseconds(tv.tv_usec);
      timer.expires_after(duration);
      list.Add(timer.async_wait(Omni::Fiber::AsioUseFiber), [channel](auto const& tuple) {
        auto [ec] = tuple;
        if (!ec) {
          ares_process_fd(channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
        }
      });
    }

    for (auto& [fd, tracker] : trackers) {
      if (tracker.ReadableInterest) {
        list.Add(
            tracker.Descriptor.async_wait(boost::asio::posix::stream_descriptor::wait_read, Omni::Fiber::AsioUseFiber),
            [channel, fd](auto const& tuple) {
              auto [ec] = tuple;
              if (!ec) {
                ares_process_fd(channel, fd, ARES_SOCKET_BAD);
              } else if (ec != boost::asio::error::operation_aborted) {
                ares_process_fd(channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
              }
            });
      }
      if (tracker.WritableInterest) {
        list.Add(
            tracker.Descriptor.async_wait(boost::asio::posix::stream_descriptor::wait_write, Omni::Fiber::AsioUseFiber),
            [channel, fd](auto const& tuple) {
              auto [ec] = tuple;
              if (!ec) {
                ares_process_fd(channel, ARES_SOCKET_BAD, fd);
              } else if (ec != boost::asio::error::operation_aborted) {
                ares_process_fd(channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
              }
            });
      }
    }

    if (list.Empty()) {
      break;
    }

    auto [listResult, cancelled] =
        co_await Omni::Fiber::Select(list, Omni::Fiber::SelectPair(cancel.GetFiberCancelEvent(), [] {}));

    // Cancel outstanding waits
    for (auto& [fd, tracker] : trackers) {
      tracker.Descriptor.cancel();
    }
    timer.cancel();

    if (cancelled) {
      err = ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
    }
  }

  ares_destroy(channel);
  co_return err;
}

} // namespace

Omni::Fiber::Coroutine<std::expected<std::vector<boost::asio::ip::address_v6>, ErrorCode>>
AresResolver::ResolveIp(boost::asio::any_io_executor executor, const std::string& host, Cancel& cancel) {
  std::expected<std::vector<boost::asio::ip::address_v6>, ErrorCode> result;

  struct ares_addrinfo_hints hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;

  auto AddrInfoCallback = [&result](int status, int timeouts, struct ares_addrinfo* list) {
    (void)timeouts;
    if (status == ARES_EDESTRUCTION) {
      return;
    }
    if (status != ARES_SUCCESS) {
      result = std::unexpected(make_error_code(boost::asio::error::host_not_found));
    } else {
      if (!result) {
        result = std::vector<boost::asio::ip::address_v6>{};
      }
      for (struct ares_addrinfo_node* node = list->nodes; node != nullptr; node = node->ai_next) {
        if (node->ai_family == AF_INET) {
          auto* addrIn = reinterpret_cast<struct sockaddr_in*>(node->ai_addr);
          boost::asio::ip::address_v4::bytes_type bytes;
          std::memcpy(bytes.data(), &addrIn->sin_addr, sizeof(bytes));
          boost::asio::ip::address_v4 addr(bytes);
          result->push_back(MapToV6(boost::asio::ip::address(addr)));
        } else if (node->ai_family == AF_INET6) {
          auto* addrIn6 = reinterpret_cast<struct sockaddr_in6*>(node->ai_addr);
          boost::asio::ip::address_v6::bytes_type bytes;
          std::memcpy(bytes.data(), &addrIn6->sin6_addr, sizeof(bytes));
          boost::asio::ip::address_v6 addr(bytes);
          result->push_back(addr);
        }
      }
    }
    if (list) {
      ares_freeaddrinfo(list);
    }
  };

  auto ec = co_await RunChannel(
      executor,
      [&](ares_channel_t* channel) -> ErrorCode {
        ares_getaddrinfo(channel, host.c_str(), nullptr, &hints,
                         LambdaBridge<decltype(AddrInfoCallback), int, int, struct ares_addrinfo*>, &AddrInfoCallback);
        return ErrorCode{};
      },
      cancel);

  if (ec) {
    co_return std::unexpected(ec);
  }

  co_return result;
}

Omni::Fiber::Coroutine<std::expected<std::vector<SrvResult>, ErrorCode>>
AresResolver::ResolveSrv(boost::asio::any_io_executor executor, const std::string& serviceName, Cancel& cancel) {
  std::expected<std::vector<SrvResult>, ErrorCode> result;
  auto SrvCallback = [&result](ares_status_t status, size_t timeouts, const ares_dns_record_t* dnsrec) {
    (void)timeouts;
    if (status == ARES_EDESTRUCTION) {
      return;
    }
    if (status != ARES_SUCCESS) {
      result = std::unexpected(make_error_code(boost::asio::error::host_not_found));
    } else if (dnsrec == nullptr) {
      result = std::unexpected(make_error_code(boost::asio::error::invalid_argument));
    } else {
      if (!result) {
        result = std::vector<SrvResult>{};
      }
      size_t count = ares_dns_record_rr_cnt(dnsrec, ARES_SECTION_ANSWER);
      for (size_t idx = 0; idx < count; ++idx) {
        const ares_dns_rr_t* rr = ares_dns_record_rr_get_const(dnsrec, ARES_SECTION_ANSWER, idx);
        if (rr && ares_dns_rr_get_type(rr) == ARES_REC_TYPE_SRV) {
          const char* target = ares_dns_rr_get_str(rr, ARES_RR_SRV_TARGET);
          unsigned short port = ares_dns_rr_get_u16(rr, ARES_RR_SRV_PORT);
          if (target) {
            result->push_back({std::string(target), port});
          }
        }
      }
    }
  };

  auto ec = co_await RunChannel(
      executor,
      [&](ares_channel_t* channel) -> ErrorCode {
        ares_dns_record_t* dnsrecQuery = nullptr;
        ares_status_t queryStatus =
            ares_dns_record_create(&dnsrecQuery, 0, ARES_FLAG_RD, ARES_OPCODE_QUERY, ARES_RCODE_NOERROR);
        if (queryStatus != ARES_SUCCESS) {
          return make_error_code(boost::asio::error::no_recovery);
        }

        queryStatus = ares_dns_record_query_add(dnsrecQuery, serviceName.c_str(), ARES_REC_TYPE_SRV, ARES_CLASS_IN);
        if (queryStatus != ARES_SUCCESS) {
          ares_dns_record_destroy(dnsrecQuery);
          return make_error_code(boost::asio::error::no_recovery);
        }

        queryStatus = ares_search_dnsrec(
            channel, dnsrecQuery, LambdaBridge<decltype(SrvCallback), ares_status_t, size_t, const ares_dns_record_t*>,
            &SrvCallback);
        ares_dns_record_destroy(dnsrecQuery);

        if (queryStatus != ARES_SUCCESS) {
          return make_error_code(boost::asio::error::no_recovery);
        }

        return ErrorCode{};
      },
      cancel);

  if (ec) {
    co_return std::unexpected(ec);
  }

  co_return result;
}

} // namespace gh
