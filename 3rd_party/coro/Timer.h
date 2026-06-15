
#pragma once

#include <asio/steady_timer.hpp>
#include "coro/AsioTask.h"
#include "coro/IoService.h"

namespace coro {

/// Wrapper вокруг asio::steady_timer
class Timer {
public:
	Timer(): _handle(IoService::current()->_impl) {}

	template <typename Duration>
	void waitFor(Duration duration) {
		_handle.expires_after(duration);
		wait();
	}

	template <typename Timestamp>
	void waitUntil(Timestamp timestamp) {
		_handle.expires_at(timestamp);
		wait();
	}

private:
	void wait() {
		awaitOp([&](auto&& handler) {
			_handle.async_wait(std::forward<decltype(handler)>(handler));
		});
	}

	asio::steady_timer _handle;
};

}
