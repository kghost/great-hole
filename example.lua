u1 = hole.udp(15525)
u2 = hole.udp(14252)
c1 = u1:create_channel("127.0.0.1", 14252)
c2 = u2:create_channel("127.0.0.1", 15525)

o1 = hole.tun("testtun1")
o2 = hole.tun("testtun2")

f = hole.filter_xor("adfasfasghagertasknldgfowpgnhophgoasndgflanhgopwehtgweopgfhweiopgfhiopafnhoawenfopw")

p1 = hole.pipeline(c1, f, o1)
p2 = hole.pipeline(o1, f, c1)
p3 = hole.pipeline(c2, f, o2)
p4 = hole.pipeline(o2, f, c2)

p1:start()
p2:start()
p3:start()
p4:start()
