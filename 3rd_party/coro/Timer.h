
#pragma once

#include <asio/steady_timer.hpp>
#include "coro/detail/Engine.hpp"

namespace coro {

/// Interruptible timer (drop-in for the old coro::Timer).
///
/// Kept as a class because several places store it as a member. Internally it
/// is just an asio::steady_timer waited on through detail::await(), so it
/// participates in cancellation and deadlines like every other suspension.
class Timer {
public:
	Timer(): _handle(detail::io()) {}

	template <typename Duration>
	void waitFor(Duration duration) {
		_handle.expires_after(std::chrono::duration_cast<Clock::duration>(duration));
		wait();
	}

	template <typename Timestamp>
	void waitUntil(Timestamp timestamp) {
		_handle.expires_at(timestamp);
		wait();
	}

private:
	void wait() {
		detail::await([&](auto token) {
			return _handle.async_wait(token);
		});
	}

	asio::steady_timer _handle;
};

}
