#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "config.h"

#include "endpoint-tun.hpp"
#include "endpoint-udp.hpp"
#include "filter-xor.hpp"

#include "pipeline.hpp"
#include "pipeline-endpoint.hpp"

int main (int argc, char **argv) {
    boost::asio::io_service io_service;
	std::shared_ptr<tun> o1(new tun(io_service));
	std::shared_ptr<udp> o2(new udp(io_service, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 18913)));
	std::shared_ptr<endpoint> e1(new endpoint_object<tun>(o1));
	std::shared_ptr<endpoint> e2 = o2->create_channel(boost::asio::ip::udp::endpoint(boost::asio::ip::address_v4::from_string("127.0.0.1"), 18913));

	unsigned char key[] = {
		0x25, 0xc5, 0x5a, 0xee, 0xed, 0x1c, 0x3c, 0x19,
		0x4a, 0xc3, 0xec, 0xc5, 0x1a, 0xd7, 0x19, 0x7a,
		0x33, 0x84, 0x4a, 0x45, 0x35, 0x00, 0x84, 0x2b,
		0x0e, 0x38, 0x41, 0x6c, 0x8b, 0x39, 0xcf, 0x22,
		0xe7, 0x57, 0xf9, 0x5e, 0xf4, 0x41, 0xbc, 0x4f,
		0x60, 0x36, 0x7e, 0x6e, 0x16, 0x57, 0x22, 0xf3,
		0xc4, 0x69, 0x13, 0xa5, 0x30, 0xf6, 0xa1, 0x65,
		0x57, 0x1e, 0x70, 0xe1, 0x1c, 0xb2, 0xf0, 0xee
	};

	std::shared_ptr<filter> f1(new filter_xor(std::vector<char>(key, key + sizeof(key))));
	auto p1 = pipeline::create_pipeline(e1, e2, f1);
	auto p2 = pipeline::create_pipeline(e2, e1, f1);
	p1->start();
	p2->start();
	io_service.run();

	exit (0);
}
