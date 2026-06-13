
#pragma once

#include "coro/detail/Engine.hpp"

namespace coro {

// Thin compatibility facade over the ambient asio::io_context that
// Application::run() installs for the current thread. Kept so that the few call
// sites written as `IoService::current()->_impl` keep working unchanged.
class IoService {
public:
	static IoService* current() {
		static thread_local IoService instance;
		return &instance;
	}

	template <typename T>
	void post(T&& t) {
		asio::post(_impl, std::forward<T>(t));
	}

	template <typename T>
	void dispatch(T&& t) {
		asio::dispatch(_impl, std::forward<T>(t));
	}

	// Reference to the ambient io_context (valid only inside Application::run()).
	asio::io_context& _impl = detail::io();
};

}
