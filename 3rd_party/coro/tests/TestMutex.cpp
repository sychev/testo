
#include "coro/Mutex.h"
#include <catch.hpp>

using namespace coro;

TEST_CASE("A basic mutex test") {
	std::vector<uint8_t> actual, expected = {0, 1, 2, 3};

	Mutex mutex;

	// Адрес локальной переменной выступает токеном ручного пробуждения coro1.
	int token;

	Coro coro1([&] {
		std::lock_guard<Mutex> lock(mutex);
		actual.push_back(0);
		Coro::current()->suspend(&token);
		actual.push_back(2);
	});
	Coro coro2([&] {
		actual.push_back(1);
		std::lock_guard<Mutex> lock(mutex);
		actual.push_back(3);
	});
	coro1.start();
	coro2.start();
	coro1.wake(&token);

	REQUIRE(actual == expected);
}
