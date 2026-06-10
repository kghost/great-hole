#include "AresResolver.hpp"

#include <chrono>
#include <cstring>
#include <expected>
#include <memory>
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

template <auto MemberFunctionPtr, typename Class, typename... Args>
void MemberFunctionBridge(void* instance, Args... args) {
  (reinterpret_cast<Class*>(instance)->*MemberFunctionPtr)(args...);
}

template <typename Lambda, typename... Args> void LambdaBridge(void* data, Args... args) {
  (*reinterpret_cast<Lambda*>(data))(args...);
}

class AresChannelRunner {
public:
  explicit AresChannelRunner(boost::asio::any_io_executor executor) : _Executor(executor) {
    (void)ares_library_init(ARES_LIB_INIT_ALL);
  }

  ~AresChannelRunner() {
    if (_Channel) {
      ares_destroy(_Channel);
    }
    ares_library_cleanup();
  }

  AresChannelRunner(const AresChannelRunner&) = delete;
  AresChannelRunner& operator=(const AresChannelRunner&) = delete;
  AresChannelRunner(AresChannelRunner&&) = delete;
  AresChannelRunner& operator=(AresChannelRunner&&) = delete;

  void OnSocketStateChange(ares_socket_t fd, int readable, int writable) {
    auto it = _Trackers.find(fd);
    if (!readable && !writable) {
      if (it != _Trackers.end()) {
        _Trackers.erase(it);
      }
      return;
    }

    if (it == _Trackers.end()) {
      auto [newIt, inserted] = _Trackers.emplace(fd, std::make_unique<SocketTracker>(_Executor, fd));
      it = newIt;
    }

    auto& tracker = *it->second;
    tracker.ReadableInterest = readable;
    tracker.WritableInterest = writable;
  };

  bool Init() {
    struct ares_options options;
    int optmask = 0;
    options.sock_state_cb =
        MemberFunctionBridge<&AresChannelRunner::OnSocketStateChange, AresChannelRunner, ares_socket_t, int, int>;
    options.sock_state_cb_data = this;
    optmask |= ARES_OPT_SOCK_STATE_CB;

    int status = ares_init_options(&_Channel, &options, optmask);
    return status == ARES_SUCCESS;
  }

  ares_channel GetChannel() const { return _Channel; }

  Omni::Fiber::Coroutine<ErrorCode> RunChannel(boost::asio::any_io_executor executor,
                                               std::shared_ptr<AresChannelRunner> runner, Cancel& cancel) {
    while (true) {
      Omni::Fiber::SelectPairList<Omni::Fiber::AsioResult<boost::system::error_code>,
                                  std::function<void(std::tuple<boost::system::error_code>)>>
          list;

      boost::asio::steady_timer timer(executor);
      struct timeval tv;
      struct timeval* tvPtr = ares_timeout(runner->GetChannel(), nullptr, &tv);
      if (tvPtr) {
        auto duration = std::chrono::seconds(tv.tv_sec) + std::chrono::microseconds(tv.tv_usec);
        timer.expires_after(duration);
        list.Add(timer.async_wait(Omni::Fiber::AsioUseFiber), [this](auto const& tuple) {
          auto [ec] = tuple;
          if (!ec) {
            ares_process_fd(_Channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
          }
        });
      }

      for (auto const& [fd, tracker] : _Trackers) {
        if (tracker->ReadableInterest) {
          list.Add(tracker->Descriptor.async_wait(boost::asio::posix::stream_descriptor::wait_read,
                                                  Omni::Fiber::AsioUseFiber),
                   [this, fd](auto const& tuple) {
                     auto [ec] = tuple;
                     if (!ec) {
                       ares_process_fd(_Channel, fd, ARES_SOCKET_BAD);
                     } else if (ec != boost::asio::error::operation_aborted) {
                       ares_process_fd(_Channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
                     }
                   });
        }
        if (tracker->WritableInterest) {
          list.Add(tracker->Descriptor.async_wait(boost::asio::posix::stream_descriptor::wait_write,
                                                  Omni::Fiber::AsioUseFiber),
                   [this, fd](auto const& tuple) {
                     auto [ec] = tuple;
                     if (!ec) {
                       ares_process_fd(_Channel, ARES_SOCKET_BAD, fd);
                     } else if (ec != boost::asio::error::operation_aborted) {
                       ares_process_fd(_Channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
                     }
                   });
        }
      }

      if (list.Empty()) {
        co_return ErrorCode{};
      }

      auto [listResult, cancelled] =
          co_await Omni::Fiber::Select(list, Omni::Fiber::SelectPair(cancel.GetFiberCancelEvent(), [] {}));

      // Cancel outstanding waits
      for (auto const& [fd, tracker] : _Trackers) {
        tracker->Descriptor.cancel();
      }
      timer.cancel();

      if (cancelled) {
        co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
      }
    }
  }

private:
  struct SocketTracker {
    explicit SocketTracker(boost::asio::any_io_executor executor, ares_socket_t fd) : Descriptor(executor) {
      Descriptor.assign(fd);
    }

    ~SocketTracker() {
      Descriptor.cancel();
      Descriptor.release();
    }

    boost::asio::posix::stream_descriptor Descriptor;
    bool ReadableInterest = false;
    bool WritableInterest = false;
  };

  boost::asio::any_io_executor _Executor;
  ares_channel _Channel = nullptr;
  std::unordered_map<ares_socket_t, std::unique_ptr<SocketTracker>> _Trackers;
};

} // namespace

Omni::Fiber::Coroutine<std::expected<std::vector<boost::asio::ip::address_v6>, ErrorCode>>
AresResolver::ResolveIp(boost::asio::any_io_executor executor, const std::string& host, Cancel& cancel) {
  if (cancel.IsTriggered()) {
    co_return std::unexpected(ErrorCode{AppErrorCategory::kOperationAborted, kAppError});
  }

  auto runner = std::make_shared<AresChannelRunner>(executor);
  if (!runner->Init()) {
    co_return std::unexpected(make_error_code(boost::asio::error::no_recovery));
  }

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

  ares_getaddrinfo(runner->GetChannel(), host.c_str(), nullptr, &hints,
                   LambdaBridge<decltype(AddrInfoCallback), int, int, struct ares_addrinfo*>, &AddrInfoCallback);

  auto ec = co_await runner->RunChannel(executor, runner, cancel);
  if (ec) {
    co_return std::unexpected(ec);
  }

  co_return result;
}

Omni::Fiber::Coroutine<std::expected<std::vector<SrvResult>, ErrorCode>>
AresResolver::ResolveSrv(boost::asio::any_io_executor executor, const std::string& serviceName, Cancel& cancel) {
  if (cancel.IsTriggered()) {
    co_return std::unexpected(ErrorCode{AppErrorCategory::kOperationAborted, kAppError});
  }

  auto runner = std::make_shared<AresChannelRunner>(executor);
  if (!runner->Init()) {
    co_return std::unexpected(make_error_code(boost::asio::error::no_recovery));
  }

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

  ares_dns_record_t* dnsrecQuery = nullptr;
  ares_status_t queryStatus =
      ares_dns_record_create(&dnsrecQuery, 0, ARES_FLAG_RD, ARES_OPCODE_QUERY, ARES_RCODE_NOERROR);
  if (queryStatus != ARES_SUCCESS) {
    co_return std::unexpected(make_error_code(boost::asio::error::no_recovery));
  }

  queryStatus = ares_dns_record_query_add(dnsrecQuery, serviceName.c_str(), ARES_REC_TYPE_SRV, ARES_CLASS_IN);
  if (queryStatus != ARES_SUCCESS) {
    ares_dns_record_destroy(dnsrecQuery);
    co_return std::unexpected(make_error_code(boost::asio::error::no_recovery));
  }

  queryStatus = ares_search_dnsrec(runner->GetChannel(), dnsrecQuery,
                                   LambdaBridge<decltype(SrvCallback), ares_status_t, size_t, const ares_dns_record_t*>,
                                   &SrvCallback);
  ares_dns_record_destroy(dnsrecQuery);

  if (queryStatus != ARES_SUCCESS) {
    co_return std::unexpected(make_error_code(boost::asio::error::no_recovery));
  }

  auto ec = co_await runner->RunChannel(executor, runner, cancel);
  if (ec) {
    co_return std::unexpected(ec);
  }

  co_return result;
}

} // namespace gh
