# Resolvers Module

The `resolvers` module provides asynchronous, fiber-compatible endpoint and address resolvers for the GreatHole project.

## Public APIs

All resolvers inherit from the `Resolver` base class or the service/port/endpoint base classes and perform resolution asynchronously using C++23 coroutines (`Omni::Fiber::Coroutine`).

### Types of Resolvers

1. **`ResolverStaticIp`**
   - Directly wraps a static IPv6 (or IPv4-mapped IPv6) address and returns it instantly.
   - **Usage:** `std::make_shared<ResolverStaticIp>(address)`

2. **`ResolverIpDns`**
   - Asynchronously resolves hostnames (e.g., `"localhost"` or `"example.com"`) using `WindowsAsyncResolver` (DnsQueryEx) on Windows and `AresResolver` (c-ares) on other platforms.
   - **Usage:** `std::make_shared<ResolverIpDns>(executor, hostname)`

3. **`ResolverNumberPort`**
   - Directly wraps a numeric port.
   - **Usage:** `std::make_shared<ResolverNumberPort>(port)`

4. **`ResolverServicePort`**
   - Resolves a service name (e.g., `"http"`) to a port using system service files.
   - **Usage:** `std::make_shared<ResolverServicePort>(service_name)`

5. **`ResolverCombinedEndpoint`**
   - Combines an IP resolver and a port resolver to resolve to a `boost::asio::ip::udp::endpoint`.
   - **Usage:** `std::make_shared<ResolverCombinedEndpoint>(ip_resolver, port_resolver)`

6. **`ResolverDnsService`**
   - Resolves DNS SRV records (e.g., `_sip._tcp.example.com`) to target hostnames and ports.
   - **Usage:** `std::make_shared<ResolverDnsService>(service_name, target)`

---

## Helper Functions

The module provides helper functions in `ResolverHelper.hpp` to parse and instantiate appropriate resolvers from user input strings:

- `FindResolverIp(const std::string& input, ResolveFor& target)`
- `FindResolverPort(const std::string& input, ResolveFor& target)`
- `FindResolverEndpoint(const std::string& input, ResolveFor& target)`
