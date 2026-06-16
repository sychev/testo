
#include "coro/CoroPool.h"
#include "coro/Queue.h"
#include "coro/Timer.h"
#include <catch.hpp>

using namespace coro;
using namespace std::chrono_literals;

TEST_CASE("CoroPool waits for all children", "[CoroPool]") {
	int done = 0;

	CoroPool pool;
	pool.exec([&] {
		Timer timer;
		timer.waitFor(5ms);
		++done;
	});
	pool.exec([&] {
		Timer timer;
		timer.waitFor(5ms);
		++done;
	});
	pool.waitAll();

	REQUIRE(done == 2);
}

TEST_CASE("CoroPool::cancelAll cancels blocked children", "[CoroPool]") {
	CoroPool pool;
	pool.exec([] {
		Queue<int> queue;
		queue.pop();   // блокируется навсегда
	});
	pool.exec([] {
		Queue<int> queue;
		queue.pop();
	});

	pool.cancelAll();
	REQUIRE_NOTHROW(pool.waitAll(false));
}

TEST_CASE("CoroPool destructor cancels and waits", "[CoroPool]") {
	bool started = false;
	{
		CoroPool pool;
		pool.exec([&] {
			started = true;
			Queue<int> queue;
			queue.pop();   // блокируется; деструктор пула отменит и дождётся
		});
		Timer timer;
		timer.waitFor(5ms);   // даём корутине стартовать
	}
	REQUIRE(started);
}
