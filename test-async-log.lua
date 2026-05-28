u1 = hole.udp(25525)
u2 = hole.udp(24252)
c1 = u1:create_channel("127.0.0.1", 24252)
c2 = u2:create_channel("127.0.0.1", 25525)

f = hole.filter_xor("adfasfasghagertasknldgfowpgnhophgoasndgflanhgopwehtgweopgfhweiopgfhiopafnhoawenfopw")

p1 = hole.pipeline(c1, f, c2)
p2 = hole.pipeline(c2, f, c1)

hole.wait_for_exit()

p2:stop()
p1:stop()

c2:stop()
c1:stop()

u2:stop()
u1:stop()
