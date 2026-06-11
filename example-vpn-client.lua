function string.fromhex(str)
        return (str:gsub('..', function (cc)
                return string.char(tonumber(cc, 16))
        end))
end

-- Create a TunSplitIp interface (using tun0)
tun = hole.tun("tun0")

-- Create the VpnServer manager, passing the TunSplitIp interface and a list of filters
udp = hole.udp_dyn_mux()

tun:start()
udp:start()

-- Register a peer with a 16-byte PSK and its permitted IPv6 address(es)
-- The PSK must be exactly 16 bytes.
key = "662069c972f50b26b3a75d03265e9eb1"
channel = udp:create_channel(key:fromhex(), "server:port")

-- Create a XOR filter for packet encryption/decryption
filter_key = "25c55aeeed1c3c194ac3ecc51ad7197a33844a453500842b0e38416c8b39cf22e757f95ef441bc4f60367e6e165722f3c46913a530f6a165571e70e11cb2f0ee"
xor_filter = hole.filter_xor(filter_key:fromhex())

-- Pipeline: tun <-> xor_filter <-> channel
p = hole.pipeline(tun, xor_filter, channel)

p:start()

hole.wait_for_exit()

p:stop()
udp:stop()
tun:stop()

print("VPN Client stopped.")
