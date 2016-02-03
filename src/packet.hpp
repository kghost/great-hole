#ifndef PACKET_H
#define PACKET_H

#include <boost/shared_ptr.hpp>

class packet {
	public:
		static const std::size_t MAX_PACKET_SIZE = 2048;
		packet() : data(new char[MAX_PACKET_SIZE]), sz(MAX_PACKET_SIZE) {}
		packet(const packet &p) : data(p.data), sz(p.sz) {}

		packet& operator=(const packet &p) { data = p.data; sz = p.sz; }

	public:
		boost::shared_ptr<char[]> data;
		std::size_t sz;
};

#endif /* end of include guard: PACKET_H */
