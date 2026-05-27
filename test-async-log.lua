o1 = hole.tun("testtun1")
o2 = hole.tun("testtun2")

f = hole.filter_xor("adfasfasghagertasknldgfowpgnhophgoasndgflanhgopwehtgweopgfhweiopgfhiopafnhoawenfopw")

p1 = hole.pipeline(o1, f, o2)
p2 = hole.pipeline(o2, f, o1)

hole.schedule(p1)
hole.schedule(p2)
