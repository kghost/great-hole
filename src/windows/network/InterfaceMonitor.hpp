#pragma once

#include <expected>
#include <future>
#include <map>
#include <optional>
#include <string>
#include <variant>

#include <boost/asio.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/address_v4.hpp>

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

class InterfaceMonitor : public ServiceBase, public WindowsLocalAddressMonitor {
public:
  using InterfaceKey = NET_IFINDEX;
  template <Interface::AddressTypes AddressType> using InterfaceAddress = Interface::InterfaceAddress<AddressType>;
  using IpInterfaceCommon = Interface::IpInterfaceCommon;
  template <Interface::AddressTypes AddressType> using IpInterfaceInfo = Interface::IpInterfaceInfo<AddressType>;
  using InterfaceInfo = Interface::InterfaceInfo;

  explicit InterfaceMonitor(boost::asio::any_io_executor executor);
  ~InterfaceMonitor() override;

  InterfaceMonitor(const InterfaceMonitor&) = delete;
  auto operator=(const InterfaceMonitor&) -> InterfaceMonitor& = delete;
  InterfaceMonitor(InterfaceMonitor&&) = delete;
  auto operator=(InterfaceMonitor&&) -> InterfaceMonitor& = delete;

  [[nodiscard]] auto GetName() const -> std::string override { return "InterfaceMonitor"; }

  // Retrieves the current cached snapshot of Windows network interfaces and their IP addresses.
  [[nodiscard]] auto GetInterfaces(bool refresh) -> const std::map<InterfaceKey, InterfaceInfo>&;
  [[nodiscard]] auto GetAddressInfo(const Address& address) const -> std::optional<IpAddressInfo> override;

  // Refreshes cached snapshots of IP interfaces and addresses while preserving OriginalDnsServers.
  void Refresh();

  template <Interface::AddressTypes AddressType>
  auto OverrideDnsServers(InterfaceKey interfaceIndex, const std::vector<AddressType>& dnsServers)
      -> std::expected<void, std::string>;
  auto RestoreDnsServers(InterfaceKey interfaceIndex) -> std::expected<void, std::string>;

protected:
  auto DoStart() -> Omni::Fiber::Coroutine<ErrorCode> override;
  auto DoWork() -> Omni::Fiber::Coroutine<void> override;
  auto DoGracefulStop() -> Omni::Fiber::Coroutine<ErrorCode> override;

private:
  struct AddressChangeEvent {
    MIB_NOTIFICATION_TYPE NotificationType;
    MIB_UNICASTIPADDRESS_ROW Row;
  };

  struct InterfaceChangeEvent {
    MIB_NOTIFICATION_TYPE NotificationType;
    MIB_IPINTERFACE_ROW Row;
  };

  using MonitorEvent = std::variant<AddressChangeEvent, InterfaceChangeEvent>;

  struct Task {
    MonitorEvent Event;
    std::promise<void> CompletionPromise;
  };

  auto QueryAddresses() -> std::map<Address, IpAddressInfo>;
  auto QueryIpInterfaces() -> std::map<InterfaceKey, InterfaceInfo>;
  static auto ConvertSockaddrInet(const SOCKADDR_INET& addr) -> std::optional<Address>;
  auto HandleAddressChangeEvent(AddressChangeEvent& event) -> bool;
  void HandleInterfaceChangeEvent(InterfaceChangeEvent& event);
  static void WINAPI OnUnicastIpAddressChangedCallback(PVOID context, PMIB_UNICASTIPADDRESS_ROW row,
                                                       MIB_NOTIFICATION_TYPE notificationType);
  static void WINAPI OnIpInterfaceChangedCallback(PVOID context, PMIB_IPINTERFACE_ROW row,
                                                  MIB_NOTIFICATION_TYPE notificationType);

  std::map<Address, IpAddressInfo> _Addresses;
  std::map<InterfaceKey, InterfaceInfo> _Interfaces;
  Omni::Fiber::ExternalQueue<Task> _TaskQueue;
  base::ComponentLogger _Logger{boost::log::keywords::channel = "InterfaceMonitor"};
};

} // namespace gh::windows::network
