#include <csignal>
#include <iostream>
#include <memory>

#include <boost/asio.hpp>

#include "Asio.hpp"
#include "Cancel.hpp"
#include "InterfaceMonitor.hpp"
#include "Manager.hpp"

auto main(int argc, char* argv[]) -> int {
  boost::asio::io_context ioContext;
  Omni::Fiber::AsioExecutor executor(ioContext.get_executor());
  Omni::Fiber::Manager manager(executor);
  gh::Cancel cancel;

  manager.SpawnRoot("root", [&]() -> Omni::Fiber::Coroutine<void> {
    std::shared_ptr<gh::windows::network::InterfaceMonitor> monitor;
    monitor = std::make_shared<gh::windows::network::InterfaceMonitor>(ioContext.get_executor());

    auto startErr = co_await monitor->Start();
    if (startErr) {
      std::cerr << "Failed to start InterfaceMonitor: " << startErr.message() << "\n";
      co_return;
    }

    std::cout << "Monitoring for changes... Press Ctrl+C to exit.\n";
    while (!cancel.IsTriggered()) {
      co_await cancel.GetFiberCancelEvent();
      break;
    }

    co_await monitor->Stop();
  });

  boost::asio::signal_set signals(ioContext, SIGINT, SIGTERM);
  signals.async_wait([&](const boost::system::error_code&, int) -> void {
    std::cout << "\nStopping InterfaceMonitor...\n";
    cancel.Trigger();
  });

  ioContext.run();
  return 0;
}
