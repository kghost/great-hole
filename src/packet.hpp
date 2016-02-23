#ifndef PACKET_H
#define PACKET_H

#include <memory>

class packet {
	public:
		static const std::size_t MAX_PACKET_SIZE = 2048;
		packet() : data(new std::array<char, MAX_PACKET_SIZE>), sz(MAX_PACKET_SIZE) {}

		packet(packet &&p) : data(std::move(p.data)), sz(std::move(p.sz)) {}
		packet& operator=(const packet &&o) { data = std::move(o.data); sz = std::move(o.sz); }

	public:
		std::shared_ptr<std::array<char, MAX_PACKET_SIZE>> data;
		std::size_t sz;
};

#endif /* end of include guard: PACKET_H */
