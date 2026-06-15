
#include "coro/Resolver.h"
#include <catch.hpp>

using namespace asio::ip;
using namespace coro;

TEST_CASE("Localhost resolving") {
	TcpResolver resolver;
	auto results = resolver.resolve(tcp::v4(), "localhost", "12345");
	REQUIRE(results.begin()->endpoint() == tcp::endpoint(make_address("127.0.0.1"), 12345));
}

TEST_CASE("'@' resolving") {
	TcpResolver resolver;
	REQUIRE_THROWS(resolver.resolve(tcp::v4(), "@", "12345"));
}
