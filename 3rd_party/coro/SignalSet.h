
#pragma once

#include <cstdint>
#include <initializer_list>
#include <asio/signal_set.hpp>
#include "coro/detail/Engine.hpp"

namespace coro {

/// Wrapper around asio::signal_set (drop-in for old coro::SignalSet).
class SignalSet {
public:
	SignalSet(const std::initializer_list<int32_t>& signals): _handle(detail::io()) {
		for (auto signal: signals) {
			_handle.add(signal);
		}
	}

	/// Suspends the coroutine until one of the signals arrives; returns its number.
	int32_t wait() {
		return detail::await([&](auto token) {
			return _handle.async_wait(token);
		});
	}

private:
	asio::signal_set _handle;
};

}
