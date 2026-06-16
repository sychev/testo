
#include "coro/Timeout.h"
#include "coro/Timer.h"
#include "coro/Queue.h"
#include "coro/Acceptor.h"
#include "coro/StreamSocket.h"
#include "coro/DatagramSocket.h"
#include "coro/CheckPoint.h"
#include <catch.hpp>
#include <thread>
#include <vector>

using namespace asio::ip;
using namespace coro;
using namespace std::chrono_literals;

TEST_CASE("Timeout interrupts a blocking wait", "[Timeout]") {
	Timeout timeout(100ms);
	Timer slow;
	REQUIRE_THROWS_AS(slow.waitFor(10s), TimeoutError);
}

TEST_CASE("TimeoutError carries the Timeout", "[Timeout]") {
	Timeout timeout(100ms);
	Timer slow;
	try {
		slow.waitFor(10s);
	}
	catch (const TimeoutError& error) {
		REQUIRE(error.timeout() == &timeout);
	}
}

TEST_CASE("Timeout fires even if the coro was busy", "[Timeout]") {
	Timeout timeout(100ms);
	std::this_thread::sleep_for(200ms);   // таймер истёк, но обработчик ещё не вызывался
	Timer slow;
	REQUIRE_THROWS_AS(slow.waitFor(10s), TimeoutError);
}

TEST_CASE("Cancelled timeout does not fire", "[Timeout]") {
	{
		Timeout timeout(100ms);
	}
	std::this_thread::sleep_for(200ms);
	REQUIRE_NOTHROW(CheckPoint());
}

TEST_CASE("Two timeouts each interrupt their wait", "[Timeout]") {
	{
		Timeout timeout(100ms);
		Timer slow;
		REQUIRE_THROWS_AS(slow.waitFor(10s), TimeoutError);
	}
	{
		Timeout timeout(100ms);
		Timer slow;
		REQUIRE_THROWS_AS(slow.waitFor(10s), TimeoutError);
	}
}

TEST_CASE("Timeout + queue", "[Timeout]") {
	Timeout timeout(100ms);
	Queue<uint64_t> queue;
	REQUIRE_THROWS_AS(queue.pop(), TimeoutError);
}

TEST_CASE("Timeout + acceptor", "[Timeout]") {
	Timeout timeout(100ms);
	auto endpoint = tcp::endpoint(make_address("127.0.0.1"), 44442);
	Acceptor<tcp> acceptor(endpoint);
	REQUIRE_THROWS_AS(acceptor.accept(), TimeoutError);
}

TEST_CASE("Timeout + TCP socket", "[Timeout]") {
	Timeout timeout(100ms);
	auto endpoint = tcp::endpoint(make_address("127.0.0.1"), 44443);
	Acceptor<tcp> acceptor(endpoint);
	StreamSocket<tcp> socket;
	socket.connect(endpoint);
	std::vector<uint8_t> buffer(10);
	REQUIRE_THROWS_AS(socket.read(asio::buffer(buffer)), TimeoutError);
}

TEST_CASE("Timeout + UDP socket", "[Timeout]") {
	Timeout timeout(100ms);
	DatagramSocket<udp> socket(udp::endpoint(make_address("127.0.0.1"), 44444));
	std::vector<uint8_t> buffer(10);
	udp::endpoint endpoint;
	REQUIRE_THROWS_AS(socket.receive(asio::buffer(buffer), endpoint), TimeoutError);
}
