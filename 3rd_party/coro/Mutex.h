
#pragma once

#include <asio/steady_timer.hpp>
#include "coro/detail/Engine.hpp"

namespace coro {

/*!
	@brief Cooperative mutex for coroutines (drop-in for old coro::Mutex).

	Use with std::lock_guard. NOT thread-safe — it only serialises coroutines
	running on the same single-threaded event loop. While the mutex is held, a
	competing lock() suspends the coroutine until unlock() releases it (or the
	coroutine is cancelled).

	Implementation: a steady_timer used as a latch. unlock() cancels it, waking
	every waiter; the waiters re-check `_locked` and exactly one proceeds.
*/
class Mutex {
public:
	Mutex(): _latch(detail::io()) {}

	void lock() {
		while (_locked) {
			_latch.expires_at(Clock::time_point::max());
			detail::suspend_quietly([&](auto token) {
				return _latch.async_wait(token);
			});
		}
		_locked = true;
	}

	void unlock() {
		_locked = false;
		_latch.cancel();   // wake the waiters; one of them will grab the lock
	}

private:
	bool _locked = false;
	asio::steady_timer _latch;
};

}
