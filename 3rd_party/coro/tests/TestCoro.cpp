
#include "coro/CoroPool.h"
#include "coro/Queue.h"
#include <catch.hpp>
#include <stdexcept>

using namespace coro;

// В MT-модели корутины запускаются через CoroPool (низкоуровневого ручного Coro API больше нет).

TEST_CASE("A coroutine runs to completion", "[Coro]") {
	bool ran = false;
	CoroPool pool;
	pool.exec([&] {
		ran = true;
	});
	pool.waitAll();
	REQUIRE(ran);
}

TEST_CASE("Coro::current() is restored after a nested pool", "[Coro]") {
	CoroPool pool;
	pool.exec([&] {
		Coro* outer = Coro::current();
		REQUIRE(outer != nullptr);
		{
			CoroPool sub;
			sub.exec([&] {
				REQUIRE(Coro::current() != nullptr);
			});
			sub.waitAll();
		}
		REQUIRE(Coro::current() == outer);
	});
	pool.waitAll();
}

TEST_CASE("Cancellation throws CancelError into a blocked coroutine", "[Coro]") {
	bool cancelled = false;
	CoroPool pool;
	pool.exec([&] {
		try {
			Queue<int> queue;
			queue.pop();   // блокируется навсегда
		}
		catch (const CancelError&) {
			cancelled = true;
		}
	})->cancel();
	pool.waitAll();
	REQUIRE(cancelled);
}

TEST_CASE("An exception from a child propagates through waitAll", "[Coro]") {
	CoroPool pool;
	pool.exec([&] {
		throw std::runtime_error("boom");
	});
	REQUIRE_THROWS_AS(pool.waitAll(false), std::runtime_error);
}
