#pragma once

#include "InterfaceCommonTypes.hpp"

namespace gh::dns::tools {

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
inline auto GetDnsForwarderConfig() -> Interface::DnsForwarderConfiguration {
  return Interface::DnsForwarderConfiguration{
      .Listeners =
          {
              {
                  .LocalEndpoint =
                      Interface::DnsEndpoint{
                          .Address = Interface::Ip4Address{.Bytes = {127, 0, 0, 1}},
                          .Port = 53053,
                      },
              },
          },
      .Upstreams =
          {
              {
                  .Name = "google_dns",
                  .ServerEndpoints =
                      {
                          Interface::DnsEndpoint{
                              .Address = Interface::Ip4Address{.Bytes = {8, 8, 8, 8}},
                              .Port = 53,
                          },
                          Interface::DnsEndpoint{
                              .Address = Interface::Ip4Address{.Bytes = {8, 8, 4, 4}},
                              .Port = 53,
                          },
                      },
              },
              {
                  .Name = "local_dns",
                  .ServerEndpoints =
                      {
                          Interface::DnsEndpoint{
                              .Address = Interface::Ip4Address{.Bytes = {223, 5, 5, 5}},
                              .Port = 53,
                          },
                          Interface::DnsEndpoint{
                              .Address = Interface::Ip4Address{.Bytes = {223, 6, 6, 6}},
                              .Port = 53,
                          },
                      },
              },
          },
      .DefaultRoute = "local_dns",
      .Routes =
          {
              {"google.com", "google_dns"},
          },
  };
}
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

} // namespace gh::dns::tools
