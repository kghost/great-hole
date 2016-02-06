bin_PROGRAMS=great-hole

great_hole_SOURCES=main.cpp pipeline.cpp endpoint-tun.cpp endpoint-udp.cpp

great_hole_CPPFLAGS=-DBOOST_ALL_DYN_LINK $(PTHREAD_CFLAGS) $(BOOST_CPPFLAGS)
great_hole_LDADD=-lpthread $(PTHREAD_LIBS) $(BOOST_LDFLAGS) $(BOOST_ASIO_LIB) $(BOOST_LOG_LIB)
