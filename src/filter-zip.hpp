#ifndef FILTER_ZIP_H
#define FILTER_ZIP_H

#include "pipeline.hpp"

namespace filter_zip {
	class compress : public filter {
		public:
			virtual packet pipe(packet &p) {
				if (compressBound(p.sz) > MAX_PACKET_SIZE) {
				} else {
					packet n;
					compress(n.data, &n.sz, p.data, p.sz);
					return n
				}
			}
	};

	class uncompress : public filter {
		public:
			virtual packet pipe(packet &p) {
				packet n;
				// XXX: fuck, zlib doesn't check output size, potential overflow attack
				uncompress(n.data, &n.sz, p.data, p.sz);
				return n
			}
	};
}

#endif /* end of include guard: FILTER_ZIP_H */
