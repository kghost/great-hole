#include "AresResolver.hpp"

#include <chrono>
#include <cstring>
#include <memory>
#include <unordered_map>

#include <ares.h>
#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <netdb.h>

#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/log/trivial.hpp>

#include "Cancel.hpp"
#include "Select.hpp"
#include "SelectPair.hpp"
#include "Utils.hpp"

namespace gh {

namespace {

class AresChannelRunner : public std::enable_shared_from_this<AresChannelRunner> {
public:
  explicit AresChannelRunner(boost::asio::any_io_executor executor) : _Executor(executor), _Timer(executor) {
    ares_library_init(ARES_LIB_INIT_ALL);
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

  bool Init() {
    struct ares_options options;
    int optmask = 0;
    options.sock_state_cb = &SocketStateCallback;
    options.sock_state_cb_data = this;
    optmask |= ARES_OPT_SOCK_STATE_CB;

    int status = ares_init_options(&_Channel, &options, optmask);
    return status == ARES_SUCCESS;
  }

  ares_channel GetChannel() const { return _Channel; }

  void UpdateTimer() {
    if (!_Channel) {
      return;
    }
    struct timeval tv;
    struct timeval* tvPtr = ares_timeout(_Channel, nullptr, &tv);
    if (!tvPtr) {
      _Timer.cancel();
      return;
    }

    auto duration = std::chrono::seconds(tv.tv_sec) + std::chrono::microseconds(tv.tv_usec);
    _Timer.expires_after(duration);
    _Timer.async_wait([selfWeak = weak_from_this()](const boost::system::error_code& ec) {
      if (!ec) {
        if (auto self = selfWeak.lock()) {
          self->ProcessTimeout();
        }
      }
    });
  }

private:
  struct SocketTracker {
    boost::asio::posix::stream_descriptor Descriptor;
    bool Reading = false;
    bool Writing = false;
    int ReadableInterest = 0;
    int WritableInterest = 0;

    explicit SocketTracker(boost::asio::any_io_executor executor, ares_socket_t fd) : Descriptor(executor) {
      Descriptor.assign(fd);
    }

    ~SocketTracker() {
      boost::system::error_code ec;
      Descriptor.cancel(ec);
      Descriptor.release();
    }
  };

  static void SocketStateCallback(void* data, ares_socket_t socketFd, int readable, int writable) {
    auto* self = static_cast<AresChannelRunner*>(data);
    self->OnSocketStateChange(socketFd, readable, writable);
  }

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

    if (readable && !tracker.Reading) {
      tracker.Reading = true;
      tracker.Descriptor.async_wait(boost::asio::posix::stream_descriptor::wait_read,
                                    [selfWeak = weak_from_this(), fd](const boost::system::error_code& ec) {
                                      if (auto self = selfWeak.lock()) {
                                        self->OnSocketEvent(fd, ec, true);
                                      }
                                    });
    }
    if (writable && !tracker.Writing) {
      tracker.Writing = true;
      tracker.Descriptor.async_wait(boost::asio::posix::stream_descriptor::wait_write,
                                    [selfWeak = weak_from_this(), fd](const boost::system::error_code& ec) {
                                      if (auto self = selfWeak.lock()) {
                                        self->OnSocketEvent(fd, ec, false);
                                      }
                                    });
    }
  }

  void OnSocketEvent(ares_socket_t fd, const boost::system::error_code& ec, bool isRead) {
    auto it = _Trackers.find(fd);
    if (it == _Trackers.end()) {
      return;
    }

    auto& tracker = *it->second;
    if (isRead) {
      tracker.Reading = false;
    } else {
      tracker.Writing = false;
    }

    if (ec) {
      if (ec != boost::asio::error::operation_aborted) {
        ares_process_fd(_Channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
        UpdateTimer();
      }
      return;
    }

    ares_socket_t rfd = isRead ? fd : ARES_SOCKET_BAD;
    ares_socket_t wfd = !isRead ? fd : ARES_SOCKET_BAD;
    ares_process_fd(_Channel, rfd, wfd);

    auto checkIt = _Trackers.find(fd);
    if (checkIt != _Trackers.end()) {
      auto& trk = *checkIt->second;
      if (trk.ReadableInterest && !trk.Reading) {
        trk.Reading = true;
        trk.Descriptor.async_wait(boost::asio::posix::stream_descriptor::wait_read,
                                  [selfWeak = weak_from_this(), fd](const boost::system::error_code& ec) {
                                    if (auto self = selfWeak.lock()) {
                                      self->OnSocketEvent(fd, ec, true);
                                    }
                                  });
      }
      if (trk.WritableInterest && !trk.Writing) {
        trk.Writing = true;
        trk.Descriptor.async_wait(boost::asio::posix::stream_descriptor::wait_write,
                                  [selfWeak = weak_from_this(), fd](const boost::system::error_code& ec) {
                                    if (auto self = selfWeak.lock()) {
                                      self->OnSocketEvent(fd, ec, false);
                                    }
                                  });
      }
    }
    UpdateTimer();
  }

  void ProcessTimeout() {
    if (!_Channel) {
      return;
    }
    ares_process_fd(_Channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
    UpdateTimer();
  }

  boost::asio::any_io_executor _Executor;
  ares_channel _Channel = nullptr;
  boost::asio::steady_timer _Timer;
  std::unordered_map<ares_socket_t, std::unique_ptr<SocketTracker>> _Trackers;
};

struct AresResolveResult {
  ErrorCode Err;
  std::vector<boost::asio::ip::address_v6> Addresses;
};

struct AresSrvResult {
  ErrorCode Err;
  std::vector<SrvResult> Records;
};

void AddrInfoCallback(void* arg, int status, int timeouts, struct ares_addrinfo* result) {
  (void)timeouts;
  if (status == ARES_EDESTRUCTION) {
    return;
  }
  auto* event = static_cast<Omni::Fiber::Event<AresResolveResult>*>(arg);
  AresResolveResult resolveResult;
  if (status != ARES_SUCCESS) {
    resolveResult.Err = make_error_code(boost::asio::error::host_not_found);
  } else if (result) {
    struct ares_addrinfo_node* node = result->nodes;
    while (node != nullptr) {
      if (node->ai_family == AF_INET) {
        auto* addrIn = reinterpret_cast<struct sockaddr_in*>(node->ai_addr);
        boost::asio::ip::address_v4::bytes_type bytes;
        std::memcpy(bytes.data(), &addrIn->sin_addr, sizeof(bytes));
        boost::asio::ip::address_v4 addr(bytes);
        resolveResult.Addresses.push_back(MapToV6(boost::asio::ip::address(addr)));
      } else if (node->ai_family == AF_INET6) {
        auto* addrIn6 = reinterpret_cast<struct sockaddr_in6*>(node->ai_addr);
        boost::asio::ip::address_v6::bytes_type bytes;
        std::memcpy(bytes.data(), &addrIn6->sin6_addr, sizeof(bytes));
        boost::asio::ip::address_v6 addr(bytes);
        resolveResult.Addresses.push_back(addr);
      }
      node = node->ai_next;
    }
  }
  if (result) {
    ares_freeaddrinfo(result);
  }
  event->Fire(std::move(resolveResult));
}

void SrvCallback(void* arg, int status, int timeouts, unsigned char* abuf, int alen) {
  (void)timeouts;
  if (status == ARES_EDESTRUCTION) {
    return;
  }
  auto* event = static_cast<Omni::Fiber::Event<AresSrvResult>*>(arg);
  AresSrvResult srvResult;
  if (status != ARES_SUCCESS) {
    srvResult.Err = make_error_code(boost::asio::error::host_not_found);
  } else {
    struct ares_srv_reply* srvResults = nullptr;
    int rc = ares_parse_srv_reply(abuf, alen, &srvResults);
    if (rc == ARES_SUCCESS) {
      struct ares_srv_reply* curr = srvResults;
      while (curr != nullptr) {
        srvResult.Records.push_back({std::string(curr->host), curr->port});
        curr = curr->next;
      }
      ares_free_data(srvResults);
    } else {
      srvResult.Err = make_error_code(boost::asio::error::invalid_argument);
    }
  }
  event->Fire(std::move(srvResult));
}

} // namespace

Omni::Fiber::Coroutine<std::expected<std::vector<boost::asio::ip::address_v6>, ErrorCode>>
AresResolver::ResolveIp(boost::asio::any_io_executor executor, const std::string& host, Cancel& cancel) {
  if (cancel.IsTriggered()) {
    co_return std::unexpected(make_error_code(boost::asio::error::operation_aborted));
  }

  auto runner = std::make_shared<AresChannelRunner>(executor);
  if (!runner->Init()) {
    co_return std::unexpected(make_error_code(boost::asio::error::no_recovery));
  }

  auto eventPtr = std::make_shared<Omni::Fiber::Event<AresResolveResult>>();
  struct ares_addrinfo_hints hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;

  ares_getaddrinfo(runner->GetChannel(), host.c_str(), nullptr, &hints, &AddrInfoCallback, eventPtr.get());
  runner->UpdateTimer();

  auto [cancelled, response] =
      co_await Omni::Fiber::Select(Omni::Fiber::SelectPair(cancel.GetFiberCancelEvent(), [] {}),
                                   Omni::Fiber::SelectPair(*eventPtr, [](auto const& res) { return res; }));

  if (cancelled) {
    co_return std::unexpected(make_error_code(boost::asio::error::operation_aborted));
  }

  if (response->Err) {
    co_return std::unexpected(response->Err);
  }

  co_return response->Addresses;
}

Omni::Fiber::Coroutine<std::expected<std::vector<SrvResult>, ErrorCode>>
AresResolver::ResolveSrv(boost::asio::any_io_executor executor, const std::string& serviceName, Cancel& cancel) {
  if (cancel.IsTriggered()) {
    co_return std::unexpected(make_error_code(boost::asio::error::operation_aborted));
  }

  auto runner = std::make_shared<AresChannelRunner>(executor);
  if (!runner->Init()) {
    co_return std::unexpected(make_error_code(boost::asio::error::no_recovery));
  }

  auto eventPtr = std::make_shared<Omni::Fiber::Event<AresSrvResult>>();

  ares_search(runner->GetChannel(), serviceName.c_str(), C_IN, T_SRV, &SrvCallback, eventPtr.get());
  runner->UpdateTimer();

  auto [cancelled, response] =
      co_await Omni::Fiber::Select(Omni::Fiber::SelectPair(cancel.GetFiberCancelEvent(), [] {}),
                                   Omni::Fiber::SelectPair(*eventPtr, [](auto const& res) { return res; }));

  if (cancelled) {
    co_return std::unexpected(make_error_code(boost::asio::error::operation_aborted));
  }

  if (response->Err) {
    co_return std::unexpected(response->Err);
  }

  co_return response->Records;
}

} // namespace gh
