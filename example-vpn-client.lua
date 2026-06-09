-- Create a TunSplitIp interface (using tun0)
tun = hole.tun("tun0")

-- Create a XOR filter for packet encryption/decryption
xor_filter = hole.filter_xor("adfasfasghagertasknldgfowpgnhophgoasndgflanhgopwehtgweopgfhweiopgfhiopafnhoawenfopw")

-- Create the VpnServer manager, passing the TunSplitIp interface and a list of filters
udp = hole.udp_dyn_mux()

-- Register a peer with a 16-byte PSK and its permitted IPv6 address(es)
-- The PSK must be exactly 16 bytes.
udp:create_channel(psk1, ips1)

-- Create a dynamic UDP multiplexer on port 25525, associating it with our VpnServer callback
udp_dyn = hole.udp_dyn_mux(25525, vpn)

print("VPN Server started on port 25525...")

-- Start the VPN Server event processing loop.
-- This blocks the current Lua fiber and processes connection/disconnection events.
vpn:run()

udp_dyn:stop()
tun_split:stop()

print("VPN Server stopped.")
