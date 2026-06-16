
#include "coro/CoroPool.h"
#include "coro/Mutex.h"
#include "coro/CheckPoint.h"
#include <catch.hpp>
#include <algorithm>

using namespace coro;

TEST_CASE("Mutex provides mutual exclusion", "[Mutex]") {
	Mutex mutex;
	int inside = 0, maxInside = 0;

	CoroPool pool;
	for (int i = 0; i < 5; ++i) {
		pool.exec([&] {
			std::lock_guard<Mutex> lock(mutex);
			++inside;
			maxInside = std::max(maxInside, inside);
			CheckPoint();   // уступаем управление, удерживая мьютекс
			--inside;
		});
	}
	pool.waitAll();

	// Если мьютекс работает, в критической секции одновременно не более одной корутины.
	REQUIRE(maxInside == 1);
}
