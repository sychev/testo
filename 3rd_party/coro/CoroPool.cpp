
#include "coro/CoroPool.h"
#include "coro/detail/Engine.hpp"

#include <vector>
#include <exception>
#include <asio/spawn.hpp>
#include <asio/steady_timer.hpp>
#include <asio/cancellation_signal.hpp>

namespace coro {

struct CoroPool::Impl {
	explicit Impl(): latch(detail::io()) {}

	int running = 0;
	std::exception_ptr pending;
	std::vector<std::shared_ptr<asio::cancellation_signal>> signals;
	asio::steady_timer latch;   // "fired" by cancelling it when a child finishes

	void cancel_all() {
		for (auto& signal: signals) {
			signal->emit(asio::cancellation_type::all);
		}
	}
};

CoroPool::CoroPool(): _impl(std::make_shared<Impl>()) {}

CoroPool::~CoroPool() {
	cancelAll();
	try {
		waitAll(true);
	} catch (...) {
		// never throw from a destructor
	}
}

void CoroPool::exec(std::function<void()> routine) {
	auto impl = _impl;
	impl->running++;

	auto signal = std::make_shared<asio::cancellation_signal>();
	impl->signals.push_back(signal);

	// Spawn the child. The lambda installs the child's own yield as the ambient
	// one while its body runs. The completion handler (called with the escaped
	// exception, if any) decrements the counter, records the first failure,
	// triggers fail-fast cancellation and wakes the parent.
	asio::spawn(detail::executor(),
		[routine = std::move(routine)](asio::yield_context yield) {
			detail::YieldScope scope(yield);
			routine();
		},
		asio::bind_cancellation_slot(signal->slot(),
			[impl](std::exception_ptr error) {
				impl->running--;
				if (error && !impl->pending) {
					impl->pending = error;
					impl->cancel_all();   // fail-fast: stop the siblings
				}
				impl->latch.cancel();     // wake waitAll()
			}));
}

void CoroPool::waitAll(bool noThrow) {
	auto impl = _impl;

	while (impl->running > 0) {
		impl->latch.expires_at(Clock::time_point::max());
		// Suspend until a finishing child cancels the latch. We deliberately use
		// the "quiet" suspend so the cancellation (operation_aborted) is not
		// turned into an exception; we just re-check the counter.
		detail::suspend_quietly([&](auto token) {
			return impl->latch.async_wait(token);
		});
	}

	if (!noThrow && impl->pending) {
		auto error = impl->pending;
		impl->pending = nullptr;
		std::rethrow_exception(error);
	}
}

void CoroPool::cancelAll() {
	_impl->cancel_all();
}

}
