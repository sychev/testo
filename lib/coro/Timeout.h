
#pragma once

#include "coro/Runtime.h"
#include <asio.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace coro {

/// Исключение, выбрасываемое при срабатывании таймаута.
class TimeoutError: public std::runtime_error {
public:
	TimeoutError(): std::runtime_error("Timeout was triggered") {}
};

/*!
	@brief Выполняет асинхронный блок с ограничением по времени.

	Заменяет прежний RAII coro::Timeout. Поскольку C++20-корутины stackless,
	нельзя «накрыть» таймаутом произвольную лексическую область — защищаемый
	участок оформляется лямбдой-корутиной:

	@code
		co_await coro::with_timeout(timeout, [&]() -> asio::awaitable<void> {
			co_await send(request);
			response = co_await recv();
		});
	@endcode

	Если время вышло раньше, чем завершилась лямбда, все её незавершённые
	co_await отменяются (asio::error::operation_aborted), и бросается TimeoutError.
*/
template <typename F>
asio::awaitable<void> with_timeout(std::chrono::nanoseconds duration, F f) {
	using namespace asio::experimental::awaitable_operators;

	asio::steady_timer timer(co_await asio::this_coro::executor);
	timer.expires_after(duration);

	auto which = co_await (
		std::move(f)() ||
		timer.async_wait(asio::use_awaitable)
	);

	if (which.index() == 1) {
		throw TimeoutError();
	}
}

}
