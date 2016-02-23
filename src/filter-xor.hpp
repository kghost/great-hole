#ifndef FILTER_XOR_H
#define FILTER_XOR_H

#include "filter.hpp"

class filter_xor : public filter {
		public:
			filter_xor(std::vector<char> const &key) : key(key) {}
			
			virtual packet pipe(packet &p) {
				for (int i = 0; i < p.sz; ++i) {
					(*p.data)[i] = (*p.data)[i] ^ key[i % key.size()];
				}
				return std::move(p);
			}

		private:
			const std::vector<char> key;
};

#endif /* end of include guard: FILTER_XOR_H */
