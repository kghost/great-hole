#ifndef FILTER_XOR_H
#define FILTER_XOR_H

#include <boost/asio/buffer.hpp>

#include "filter.hpp"

class filter_xor : public filter_symmetric<filter_xor> {
		public:
			filter_xor(std::vector<char> const &key) : key(key) {}
			
			virtual packet pipe(packet &&p) {
				auto & buffer = p.first;
				auto data = buffer.data;

				for (auto i = 0; i < buffer.length; ++i) {
					data[buffer.offset + i] = data[buffer.offset + i] ^ key[i % key.size()];
				}
				return std::move(p);
			}

		private:
			const std::vector<char> key;
};

#endif /* end of include guard: FILTER_XOR_H */
