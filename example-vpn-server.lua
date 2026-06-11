-- Create a TunSplitIp interface (using tun0)
tun_split = hole.tun_split_ip("tun0")

-- Create a XOR filter for packet encryption/decryption
xor_filter = hole.filter_xor("adfasfasghagertasknldgfowpgnhophgoasndgflanhgopwehtgweopgfhweiopgfhiopafnhoawenfopw")

-- Create a dynamic UDP multiplexer on port 25525
udp_dyn = hole.udp_dyn_mux(25525)

-- Create the VpnServer manager, passing the TunSplitIp interface, UdpDynMux, and a list of filters
vpn = hole.vpn_server(tun_split, udp_dyn, { xor_filter })

-- Register a peer with a 16-byte PSK and its permitted IPv6 address(es)
-- The PSK must be exactly 16 bytes.
local psk1 = "1234567890123456"
local ips1 = { "fd00::1" }
vpn:register_peer(psk1, ips1)

local psk2 = "abcdefghijklmnop"
local ips2 = { "fd00::2", "fd00::3" }
vpn:register_peer(psk2, ips2)

tun_split:start()
udp_dyn:start()
vpn:start()

print("VPN Server started on port 25525...")

hole.wait_for_exit()

vpn:stop()
udp_dyn:stop()
tun_split:stop()

print("VPN Server stopped.")
