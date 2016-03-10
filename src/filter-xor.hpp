#ifndef FILTER_XOR_H
#define FILTER_XOR_H

#include <boost/asio/buffer.hpp>

#include "filter.hpp"

class filter_xor : public filter_symmetric<filter_xor> {
		public:
			filter_xor(std::vector<char> const &key) : key(key) {}
			
			virtual packet pipe(packet &&p) {
				auto sz = boost::asio::buffer_size(p.first);
				auto s = boost::asio::buffer_cast<unsigned char*>(p.first);

				for (int i = 0; i < sz; ++i) {
					s[i] = s[i] ^ key[i % key.size()];
				}
				return p;
			}

		private:
			const std::vector<char> key;
};

#endif /* end of include guard: FILTER_XOR_H */
