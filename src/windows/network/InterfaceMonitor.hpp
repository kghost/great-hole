#pragma once

#include <boost/asio/ip/address_v4.hpp>
#include <future>
#include <optional>
#include <string>

#include <boost/asio.hpp>
#include <boost/asio/ip/address.hpp>

#include <windows.h>

#include <winsock2.h>

#include <iphlpapi.h>
#include <netioapi.h>
#include <ws2tcpip.h>

#include "ExternalQueue.hpp"
#include "Logger.hpp"
#include "ServiceBase.hpp"
#include "TunnelDataPlane.hpp"

namespace gh::windows::network {

class InterfaceMonitor : public gh::ServiceBase, public WindowsLocalAddressMonitor {
public:
  explicit InterfaceMonitor(boost::asio::any_io_executor executor);
  ~InterfaceMonitor() override;

  InterfaceMonitor(const InterfaceMonitor&) = delete;
  auto operator=(const InterfaceMonitor&) -> InterfaceMonitor& = delete;
  InterfaceMonitor(InterfaceMonitor&&) = delete;
  auto operator=(InterfaceMonitor&&) -> InterfaceMonitor& = delete;

  [[nodiscard]] auto GetName() const -> std::string override { return "InterfaceMonitor"; }

  // Retrieves the current cached snapshot of Windows network interfaces and their IP addresses.
  [[nodiscard]] auto GetAddresses() const -> const std::map<Address, IpAddressInfo>&;
  [[nodiscard]] auto GetAddressInfo(const Address& address) const -> std::optional<IpAddressInfo> override;

protected:
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoWork() -> Omni::Fiber::Coroutine<void> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

private:
  struct AddressChangeEvent {
    MIB_NOTIFICATION_TYPE NotificationType;
    MIB_UNICASTIPADDRESS_ROW Row;
  };

  struct Task {
    AddressChangeEvent Event;
    std::promise<void> CompletionPromise;
  };

  auto QueryInterfaces() -> std::map<Address, IpAddressInfo>;
  static auto ConvertSockaddrInet(const SOCKADDR_INET& addr) -> std::optional<Address>;
  auto HandleAddressChangeEvent(AddressChangeEvent& event) -> bool;
  static void WINAPI OnUnicastIpAddressChangedCallback(PVOID context, PMIB_UNICASTIPADDRESS_ROW row,
                                                       MIB_NOTIFICATION_TYPE notificationType);

  std::map<Address, IpAddressInfo> _Addresses;
  Omni::Fiber::ExternalQueue<Task> _TaskQueue;
  gh::base::ComponentLogger _Logger{boost::log::keywords::channel = "InterfaceMonitor"};
};

} // namespace gh::windows::network
