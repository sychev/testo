
#pragma once

#include "coro/Runtime.h"
#include <asio/signal_set.hpp>
#include <asio/use_awaitable.hpp>
#include <cstdint>
#include <initializer_list>

namespace coro {

class SignalSet {
public:
	SignalSet(const std::initializer_list<int32_t>& signals): _handle(current_executor()) {
		for (auto signal: signals) {
			_handle.add(signal);
		}
	}

	asio::awaitable<int32_t> wait() {
		co_return co_await _handle.async_wait(asio::use_awaitable);
	}

private:
	asio::signal_set _handle;
};

}
