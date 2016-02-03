#ifndef FILTER_XOR_H
#define FILTER_XOR_H

#include "pipeline.hpp"

class filter_xor : public filter {
		public:
			filter_xor(const std::vector<char> &&key) : key(key) {}
			
			virtual packet pipe(packet &p) {
				for (int i = 0; i < p.sz; ++i) {
					p.data.get()[i] = p.data.get()[i] ^ key[i % key.size()];
				}
				return p;
			}

		private:
			const std::vector<char> key;
};

#endif /* end of include guard: FILTER_XOR_H */
