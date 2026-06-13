
#pragma once

#include "coro/detail/Engine.hpp"

namespace coro {

// TimeoutError is declared in detail/Engine.hpp (so that detail::await can throw
// it). It is re-exported here for callers that include only Timeout.h.

/*!
	@brief Scoped deadline (drop-in for the old coro::Timeout).

	While this object is alive, every suspension below it (Timer, sockets,
	CheckPoint, ...) is bounded by the deadline and throws TimeoutError when it
	elapses. Deadlines nest: the nearest (earliest) one always wins.

	Implementation note: unlike the old version, this no longer arms its own
	timer. It simply pushes an absolute deadline onto a thread-local stack;
	detail::await() enforces it with asio::cancel_after on each operation. This
	removes a whole class of "timer fired between operations" races.

	@warning As before, this declares a VARIABLE, not a function:
	@code
		coro::Timeout timeout(std::chrono::seconds(10));
	@endcode
*/
class Timeout {
public:
	template <typename Duration>
	explicit Timeout(Duration duration) {
		auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(duration);
		// Collapse with the parent deadline: a nested scope can only tighten it.
		if (auto parent = detail::current_deadline(); parent && *parent < deadline) {
			deadline = *parent;
		}
		detail::deadline_stack.push_back(deadline);
	}

	~Timeout() {
		if (!detail::deadline_stack.empty()) {
			detail::deadline_stack.pop_back();
		}
	}

	Timeout(const Timeout&) = delete;
	Timeout& operator=(const Timeout&) = delete;
};

}
