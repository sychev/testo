
#pragma once

#include "coro/Runtime.h"
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>

namespace coro {

/// Обёртка вокруг asio::steady_timer.
class Timer {
public:
	Timer(): _handle(current_executor()) {}
	explicit Timer(const executor_type& ex): _handle(ex) {}

	template <typename Duration>
	asio::awaitable<void> waitFor(Duration duration) {
		_handle.expires_after(duration);
		co_await _handle.async_wait(asio::use_awaitable);
	}

	template <typename Timestamp>
	asio::awaitable<void> waitUntil(Timestamp timestamp) {
		_handle.expires_at(timestamp);
		co_await _handle.async_wait(asio::use_awaitable);
	}

	asio::steady_timer& handle() {
		return _handle;
	}

private:
	asio::steady_timer _handle;
};

}
