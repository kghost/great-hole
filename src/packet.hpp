#ifndef PACKET_H
#define PACKET_H

#include <any>

#include <boost/asio/buffer.hpp>
#include <boost/core/noncopyable.hpp>

class buffer {
public:
	static constexpr const std::size_t reserved_front = 2;

	template<std::size_t N> buffer(std::array<uint8_t, N> & storage) {
		data = storage.data();
		capacity = storage.size();
		offset = reserved_front;
		length = 0;
	}

	buffer(std::string & s) {
		data = reinterpret_cast<uint8_t*>(s.data());
		capacity = s.size();
		offset = 0;
		length = s.size();
	}

	buffer(buffer&&) = default;
	buffer& operator=(buffer&&) = default;
	buffer(const buffer&) = delete;
	buffer& operator=(const buffer&) = delete;

	explicit operator boost::asio::const_buffer() {
		return {data + offset, length};
	}

	explicit operator boost::asio::mutable_buffer() {
		return {data + offset, capacity - offset};
	}

	//  data
	//   | reserved_front |    data    | unused back |
	//
	//   |<-   offset   ->|<- length ->|
	//   |<-                 capacity              ->|
	//  
	//
	uint8_t * data;
	std::size_t capacity;
	std::size_t offset;
	std::size_t length;
};

// packet.second stores a object which holds the owner of buffer
typedef std::pair<buffer, std::any> packet;

#endif /* end of include guard: PACKET_H */
