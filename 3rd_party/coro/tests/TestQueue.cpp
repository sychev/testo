
#include "coro/CoroPool.h"
#include "coro/Queue.h"
#include <catch.hpp>
#include <vector>

using namespace coro;

TEST_CASE("Queue passes items between coroutines", "[Queue]") {
	Queue<int> queue;
	std::vector<int> got;

	CoroPool pool;
	pool.exec([&] {
		for (int i = 0; i < 4; ++i) {
			got.push_back(queue.pop());
		}
	});
	pool.exec([&] {
		for (int i = 0; i < 4; ++i) {
			queue.push(i);
		}
	});
	pool.waitAll();

	REQUIRE(got == std::vector<int>{0, 1, 2, 3});
}

TEST_CASE("Cancelling a coroutine blocked on Queue::pop", "[Queue]") {
	Queue<int> queue;
	bool cancelled = false;

	CoroPool pool;
	pool.exec([&] {
		try {
			queue.pop();
		}
		catch (const CancelError&) {
			cancelled = true;
		}
	})->cancel();
	pool.waitAll();

	REQUIRE(cancelled);
}
