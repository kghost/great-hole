#ifndef PACKET_H
#define PACKET_H

#include <boost/any.hpp>

namespace boost {
	namespace asio {
		class mutable_buffers_1;
		class const_buffers_1;
	}
}

// packet.second stores a object which holds the owner of buffer
typedef std::pair<boost::asio::mutable_buffers_1, boost::any> packet;

#endif /* end of include guard: PACKET_H */
