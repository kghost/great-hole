#include "WindowsAsyncResolver.hpp"

#include <windns.h>
#include <windows.h>

#include <atomic>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <cstring>
#include <cwchar>
#include <memory>
#include <thread>
#include <vector>

#include "Asio.hpp"
#include "Cancel.hpp"
#include "ErrorCode.hpp"
#include "Select.hpp"
#include "SelectPair.hpp"
#include "Utils.hpp"

namespace gh {

namespace {

auto ToWString(const std::string& str) -> std::wstring {
  if (str.empty()) {
    return L"";
  }
  int sizeNeeded = ::MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), nullptr, 0);
  std::wstring wstr(sizeNeeded, 0);
  ::MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstr[0], sizeNeeded);
  return wstr;
}

auto ToString(PCWSTR wstr) -> std::string {
  if (!wstr || *wstr == 0) {
    return "";
  }
  int len = (int)wcslen(wstr);
  int sizeNeeded = ::WideCharToMultiByte(CP_UTF8, 0, wstr, len, nullptr, 0, nullptr, nullptr);
  std::string str(sizeNeeded, 0);
  ::WideCharToMultiByte(CP_UTF8, 0, wstr, len, &str[0], sizeNeeded, nullptr, nullptr);
  return str;
}

class DnsWorkerThread {
public:
  static auto Get() -> DnsWorkerThread& {
    static DnsWorkerThread instance;
    return instance;
  }

  auto QueueAPC(PAPCFUNC pfnAPC, ULONG_PTR dwData) -> bool {
    return ::QueueUserAPC(pfnAPC, _Thread.native_handle(), dwData) != 0;
  }

private:
  DnsWorkerThread() {
    _Thread = std::thread([this]() -> void {
      while (_Running) {
        ::SleepEx(10, TRUE); // Process APCs alertably
      }
    });
  }

  ~DnsWorkerThread() {
    _Running = false;
    ::QueueUserAPC([](ULONG_PTR) -> void {}, _Thread.native_handle(), 0);
    if (_Thread.joinable()) {
      _Thread.join();
    }
  }

  std::thread _Thread;
  std::atomic<bool> _Running{true};
};

struct IpQueryState {
  boost::asio::any_io_executor executor;
  Omni::Fiber::Event<void> doneEvent;

  DNS_QUERY_CANCEL cancelHandleA{};
  DNS_QUERY_RESULT queryResultsA{};
  DNS_STATUS statusA = ERROR_SUCCESS;
  std::atomic<bool> pendingA{false};

  DNS_QUERY_CANCEL cancelHandleAAAA{};
  DNS_QUERY_RESULT queryResultsAAAA{};
  DNS_STATUS statusAAAA = ERROR_SUCCESS;
  std::atomic<bool> pendingAAAA{false};

  std::atomic<int> activeQueries{0};
  std::atomic<bool> completed{false};
  std::atomic<bool> cancelled{false};

  explicit IpQueryState(boost::asio::any_io_executor exec) : executor(std::move(exec)) {
    queryResultsA.Version = DNS_QUERY_RESULTS_VERSION1;
    queryResultsAAAA.Version = DNS_QUERY_RESULTS_VERSION1;
  }

  ~IpQueryState() {
    if (queryResultsA.pQueryRecords) {
      ::DnsRecordListFree(reinterpret_cast<PDNS_RECORD>(queryResultsA.pQueryRecords), DnsFreeRecordListDeep);
    }
    if (queryResultsAAAA.pQueryRecords) {
      ::DnsRecordListFree(reinterpret_cast<PDNS_RECORD>(queryResultsAAAA.pQueryRecords), DnsFreeRecordListDeep);
    }
  }
};

struct IpSubQuery {
  std::shared_ptr<IpQueryState> parent;
  bool isAAAA;
};

struct IpQueryArgs {
  std::wstring host;
  std::shared_ptr<IpQueryState> state;
};

VOID WINAPI IpQueryCompleteCallback(PVOID pContext, PDNS_QUERY_RESULT pQueryResults) {
  std::unique_ptr<std::shared_ptr<IpSubQuery>> subQueryHolder(static_cast<std::shared_ptr<IpSubQuery>*>(pContext));
  const auto& subQuery = *subQueryHolder;
  auto parent = subQuery->parent;

  if (subQuery->isAAAA) {
    parent->statusAAAA = pQueryResults->QueryStatus;
    parent->queryResultsAAAA.pQueryRecords = pQueryResults->pQueryRecords;
  } else {
    parent->statusA = pQueryResults->QueryStatus;
    parent->queryResultsA.pQueryRecords = pQueryResults->pQueryRecords;
  }

  boost::asio::post(parent->executor, [parent]() {
    parent->activeQueries--;
    if (parent->activeQueries <= 0) {
      parent->completed = true;
      parent->doneEvent.Fire();
    }
  });
}

struct SrvQueryState {
  boost::asio::any_io_executor executor;
  Omni::Fiber::Event<void> doneEvent;

  DNS_QUERY_CANCEL cancelHandle{};
  DNS_QUERY_RESULT queryResults{};
  DNS_STATUS status = ERROR_SUCCESS;
  std::atomic<bool> pending{false};
  std::atomic<bool> completed{false};
  std::atomic<bool> cancelled{false};

  explicit SrvQueryState(boost::asio::any_io_executor exec) : executor(std::move(exec)) {
    queryResults.Version = DNS_QUERY_RESULTS_VERSION1;
  }

  ~SrvQueryState() {
    if (queryResults.pQueryRecords) {
      ::DnsRecordListFree(reinterpret_cast<PDNS_RECORD>(queryResults.pQueryRecords), DnsFreeRecordListDeep);
    }
  }
};

struct SrvQueryArgs {
  std::wstring serviceName;
  std::shared_ptr<SrvQueryState> state;
};

VOID WINAPI SrvQueryCompleteCallback(PVOID pContext, PDNS_QUERY_RESULT pQueryResults) {
  std::unique_ptr<std::shared_ptr<SrvQueryState>> stateHolder(static_cast<std::shared_ptr<SrvQueryState>*>(pContext));
  auto state = *stateHolder;

  state->status = pQueryResults->QueryStatus;
  state->queryResults.pQueryRecords = pQueryResults->pQueryRecords;

  boost::asio::post(state->executor, [state]() {
    state->completed = true;
    state->doneEvent.Fire();
  });
}

} // namespace

auto WindowsAsyncResolver::ResolveIp(boost::asio::any_io_executor executor, const std::string& host, Cancel& cancel)
    -> Omni::Fiber::Coroutine<std::expected<std::vector<boost::asio::ip::address_v6>, ErrorCode>> {
  if (cancel.IsTriggered()) {
    co_return std::unexpected(Error(AppErrorCategory::kOperationAborted));
  }

  auto workGuard = boost::asio::make_work_guard(executor);
  auto state = std::make_shared<IpQueryState>(executor);
  std::wstring wHost = ToWString(host);

  state->activeQueries = 2;

  auto* args = new IpQueryArgs{.host = wHost, .state = state};
  DnsWorkerThread::Get().QueueAPC(
      [](ULONG_PTR dwParam) -> void {
        std::unique_ptr<IpQueryArgs> args(reinterpret_cast<IpQueryArgs*>(dwParam));
        auto state = args->state;

        if (state->cancelled) {
          boost::asio::post(state->executor, [state]() {
            state->activeQueries -= 2;
            if (state->activeQueries <= 0) {
              state->completed = true;
              state->doneEvent.Fire();
            }
          });
          return;
        }

        // Initiate A query
        DNS_QUERY_REQUEST requestA{};
        requestA.Version = DNS_QUERY_REQUEST_VERSION1;
        requestA.QueryName = args->host.c_str();
        requestA.QueryType = DNS_TYPE_A;
        requestA.QueryOptions = DNS_QUERY_STANDARD;

        auto subQueryA = std::make_shared<IpSubQuery>(state, false);
        auto* contextA = new std::shared_ptr<IpSubQuery>(subQueryA);
        requestA.pQueryCompletionCallback = IpQueryCompleteCallback;
        requestA.pQueryContext = contextA;

        // Initiate AAAA query
        DNS_QUERY_REQUEST requestAAAA{};
        requestAAAA.Version = DNS_QUERY_REQUEST_VERSION1;
        requestAAAA.QueryName = args->host.c_str();
        requestAAAA.QueryType = DNS_TYPE_AAAA;
        requestAAAA.QueryOptions = DNS_QUERY_STANDARD;

        auto subQueryAAAA = std::make_shared<IpSubQuery>(state, true);
        auto* contextAAAA = new std::shared_ptr<IpSubQuery>(subQueryAAAA);
        requestAAAA.pQueryCompletionCallback = IpQueryCompleteCallback;
        requestAAAA.pQueryContext = contextAAAA;

        DNS_STATUS statusA = ::DnsQueryEx(&requestA, &state->queryResultsA, &state->cancelHandleA);
        if (statusA == DNS_REQUEST_PENDING) {
          state->pendingA = true;
        } else {
          delete contextA;
          state->statusA = statusA;
          boost::asio::post(state->executor, [state]() {
            state->activeQueries--;
            if (state->activeQueries <= 0) {
              state->completed = true;
              state->doneEvent.Fire();
            }
          });
        }

        DNS_STATUS statusAAAA = ::DnsQueryEx(&requestAAAA, &state->queryResultsAAAA, &state->cancelHandleAAAA);
        if (statusAAAA == DNS_REQUEST_PENDING) {
          state->pendingAAAA = true;
        } else {
          delete contextAAAA;
          state->statusAAAA = statusAAAA;
          boost::asio::post(state->executor, [state]() {
            state->activeQueries--;
            if (state->activeQueries <= 0) {
              state->completed = true;
              state->doneEvent.Fire();
            }
          });
        }
      },
      reinterpret_cast<ULONG_PTR>(args));

  if (state->activeQueries > 0) {
    auto [listResult, cancelled] =
        co_await Omni::Fiber::Select(Omni::Fiber::SelectPair(state->doneEvent, []() {}),
                                     Omni::Fiber::SelectPair(cancel.GetFiberCancelEvent(), []() {}));

    if (cancelled) {
      state->cancelled = true;
      if (state->pendingA) {
        ::DnsCancelQuery(&state->cancelHandleA);
      }
      if (state->pendingAAAA) {
        ::DnsCancelQuery(&state->cancelHandleAAAA);
      }

      while (state->activeQueries > 0) {
        co_await state->doneEvent;
      }

      co_return std::unexpected(Error(AppErrorCategory::kOperationAborted));
    }
  }

  std::vector<boost::asio::ip::address_v6> addresses;

  if (state->statusA == ERROR_SUCCESS && (state->queryResultsA.pQueryRecords != nullptr)) {
    for (auto* record = reinterpret_cast<PDNS_RECORDW>(state->queryResultsA.pQueryRecords); record != nullptr;
         record = record->pNext) {
      if (record->wType == DNS_TYPE_A) {
        boost::asio::ip::address_v4::bytes_type bytes;
        std::memcpy(bytes.data(), &record->Data.A.IpAddress, 4);
        boost::asio::ip::address_v4 addr(bytes);
        addresses.push_back(MapToV6(boost::asio::ip::address(addr)));
      }
    }
  }

  if (state->statusAAAA == ERROR_SUCCESS && (state->queryResultsAAAA.pQueryRecords != nullptr)) {
    for (auto* record = reinterpret_cast<PDNS_RECORDW>(state->queryResultsAAAA.pQueryRecords); record != nullptr;
         record = record->pNext) {
      if (record->wType == DNS_TYPE_AAAA) {
        boost::asio::ip::address_v6::bytes_type bytes;
        std::memcpy(bytes.data(), &record->Data.AAAA.Ip6Address, 16);
        boost::asio::ip::address_v6 addr(bytes);
        addresses.push_back(addr);
      }
    }
  }

  if (addresses.empty()) {
    co_return std::unexpected(make_error_code(boost::asio::error::host_not_found));
  }

  co_return addresses;
}

auto WindowsAsyncResolver::ResolveSrv(boost::asio::any_io_executor executor, const std::string& serviceName,
                                      Cancel& cancel)
    -> Omni::Fiber::Coroutine<std::expected<std::vector<SrvResult>, ErrorCode>> {
  if (cancel.IsTriggered()) {
    co_return std::unexpected(Error(AppErrorCategory::kOperationAborted));
  }

  auto workGuard = boost::asio::make_work_guard(executor);
  auto state = std::make_shared<SrvQueryState>(executor);
  std::wstring wServiceName = ToWString(serviceName);

  auto* args = new SrvQueryArgs{wServiceName, state};
  DnsWorkerThread::Get().QueueAPC(
      [](ULONG_PTR dwParam) {
        std::unique_ptr<SrvQueryArgs> args(reinterpret_cast<SrvQueryArgs*>(dwParam));
        auto state = args->state;

        if (state->cancelled) {
          boost::asio::post(state->executor, [state]() {
            state->completed = true;
            state->doneEvent.Fire();
          });
          return;
        }

        DNS_QUERY_REQUEST request{};
        request.Version = DNS_QUERY_REQUEST_VERSION1;
        request.QueryName = args->serviceName.c_str();
        request.QueryType = DNS_TYPE_SRV;
        request.QueryOptions = DNS_QUERY_STANDARD;

        auto* context = new std::shared_ptr<SrvQueryState>(state);
        request.pQueryCompletionCallback = SrvQueryCompleteCallback;
        request.pQueryContext = context;

        DNS_STATUS status = ::DnsQueryEx(&request, &state->queryResults, &state->cancelHandle);
        if (status == DNS_REQUEST_PENDING) {
          state->pending = true;
        } else {
          delete context;
          state->status = status;
          boost::asio::post(state->executor, [state]() {
            state->completed = true;
            state->doneEvent.Fire();
          });
        }
      },
      reinterpret_cast<ULONG_PTR>(args));

  if (!state->completed) {
    auto [listResult, cancelled] =
        co_await Omni::Fiber::Select(Omni::Fiber::SelectPair(state->doneEvent, []() {}),
                                     Omni::Fiber::SelectPair(cancel.GetFiberCancelEvent(), []() {}));

    if (cancelled) {
      state->cancelled = true;
      if (state->pending) {
        ::DnsCancelQuery(&state->cancelHandle);
      }

      while (!state->completed) {
        co_await state->doneEvent;
      }

      co_return std::unexpected(Error(AppErrorCategory::kOperationAborted));
    }
  }

  if (state->status != ERROR_SUCCESS) {
    co_return std::unexpected(make_error_code(boost::asio::error::host_not_found));
  }

  std::vector<SrvResult> results;
  if (state->queryResults.pQueryRecords) {
    for (auto* record = reinterpret_cast<PDNS_RECORDW>(state->queryResults.pQueryRecords); record != nullptr;
         record = record->pNext) {
      if (record->wType == DNS_TYPE_SRV) {
        std::string target = ToString(record->Data.Srv.pNameTarget);
        uint16_t port = record->Data.Srv.wPort;
        if (!target.empty()) {
          results.push_back({target, port});
        }
      }
    }
  }

  if (results.empty()) {
    co_return std::unexpected(make_error_code(boost::asio::error::host_not_found));
  }

  co_return results;
}

} // namespace gh
