u1 = hole.udp(15525)
u2 = hole.udp(14252)
c1 = u1:create_channel("127.0.0.1", 14252)
c2 = u2:create_channel("127.0.0.1", 15525)

f = hole.filter_xor("adfasfasghagertasknldgfowpgnhophgoasndgflanhgopwehtgweopgfhweiopgfhiopafnhoawenfopw")

p1 = hole.pipeline(c1, f, c2)
p2 = hole.pipeline(c2, f, c1)
