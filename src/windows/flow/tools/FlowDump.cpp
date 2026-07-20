#include <array>
#include <boost/asio.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/program_options.hpp>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <variant>

#include <windows.h>

#include "Asio.hpp"
#include "Fiber.hpp"
#include "GetCurrentOmniFiber.hpp"
#include "Manager.hpp"
#include "Strings.hpp"
#include "WinDivertFlowSniffer.hpp"

namespace po = boost::program_options;
using namespace gh;

namespace {

class ConsoleFlowSnifferCallback : public WinDivertFlowSnifferCallback {
public:
  auto OnFlowEstablished(FlowKey key, uint32_t pid) -> Omni::Fiber::Coroutine<void> override {
    std::cout << "[+] ESTABLISHED LocalPort: " << key
              << " | PID: " << pid << " (" << GetProcessNameAndPath(pid) << ")" << std::endl;
    co_return;
  }

  auto OnFlowDeleted(FlowKey key) -> Omni::Fiber::Coroutine<void> override {
    std::cout << "[-] DELETED LocalPort:     " << key << std::endl;
    co_return;
  }

private:

  static auto GetProcessNameAndPath(uint32_t pid) -> std::string {
    if (pid == 0) {
      return "System Idle Process";
    }
    if (pid == 4) {
      return "System";
    }
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProcess == nullptr) {
      hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    }
    if (hProcess != nullptr) {
      std::array<wchar_t, MAX_PATH * 2> path{0};
      auto size = static_cast<DWORD>(path.size());
      if (QueryFullProcessImageNameW(hProcess, 0, path.data(), &size) != 0) {
        CloseHandle(hProcess);
        auto optStr = ToString(path.data());
        if (optStr) {
          const std::string& fullPath = *optStr;
          size_t pos = fullPath.find_last_of("\\/");
          std::string name = (pos == std::string::npos) ? fullPath : fullPath.substr(pos + 1);
          return name + " (" + fullPath + ")";
        }
      }
      CloseHandle(hProcess);
    }
    return "Unknown Process";
  }
};

} // namespace

auto main(int argc, char** argv) -> int {
  po::options_description desc("Options");
  desc.add_options()("help,h", "print this message");

  po::variables_map vm;
  try {
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);
  } catch (const po::error& ex) {
    std::cout << ex.what() << std::endl;
    return 1;
  }

  if (vm.count("help")) {
    std::cout << "Usage: " << argv[0] << " [options]" << std::endl;
    std::cout << std::endl;
    std::cout << desc << std::endl;
    return 0;
  }

  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);

  auto fiberRoot = manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    Cancel stopApplication;
    Cancel stopSignals;

    ConsoleFlowSnifferCallback callback;
    auto sniffer = std::make_shared<WinDivertFlowSniffer>(io.get_executor(), callback);

    auto& current = co_await Omni::Fiber::GetCurrentOmniFiber();
    auto fiberSniffer = current.Spawn("sniffer", [&]() -> Omni::Fiber::Coroutine<void> {
      auto err = co_await sniffer->Start();
      if (err) {
        std::cerr << "Error: Failed to start flow sniffer (" << err.message() << ")." << std::endl;
        std::cerr << "Make sure you are running as Administrator / Elevated prompt." << std::endl;
        co_await sniffer->Stop();
        stopApplication.Trigger();
        stopSignals.Trigger();
        co_return;
      }

      std::cout << "WinDivert Flow Sniffer started. Monitoring system flow events..." << std::endl;
      std::cout << "Press Ctrl+C to exit." << std::endl;

      co_await stopApplication.GetFiberCancelEvent();
      co_await sniffer->Stop();
    });

    auto fiberSignal = current.Spawn("signals", [&]() -> Omni::Fiber::Coroutine<void> {
      boost::asio::signal_set signals(io.get_executor(), SIGINT, SIGTERM);
      while (!stopSignals.IsTriggered()) {
        auto [err, signalNumber] = co_await signals.async_wait(stopSignals.AsioSlot()());
        if (!err) {
          if (signalNumber == SIGINT || signalNumber == SIGTERM) {
            std::cout << "\nStopping flow sniffer..." << std::endl;
            stopApplication.Trigger();
            stopSignals.Trigger();
          }
        } else if (err == boost::asio::error::operation_aborted) {
          continue;
        } else {
          break;
        }
      }
    });

    co_await current.WaitAll();
  });

  io.run();
  return 0;
}
