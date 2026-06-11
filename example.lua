u1 = hole.udp(25525)
u2 = hole.udp(24252)

u1:start()
u2:start()

c1 = u1:create_channel("127.0.0.1", 24252)
c2 = u2:create_channel("127.0.0.1", 25525)

f = hole.filter_xor("adfasfasghagertasknldgfowpgnhophgoasndgflanhgopwehtgweopgfhweiopgfhiopafnhoawenfopw")

p = hole.pipeline(c1, f, c2)

p:start()

hole.wait_for_exit()

p:stop()

c2:stop()
c1:stop()

u2:stop()
u1:stop()
