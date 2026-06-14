#pragma once

#include <asio.hpp>
#include <coro/Coro.h>
#include <coro/IoService.h>

#include <exception>
#include <optional>
#include <string>

/*
	ВРЕМЕННЫЙ мост между нативными C++20-корутинами asio и стековыми
	корутинами библиотеки coro. Позволяет во время миграции вызывать
	asio::awaitable<T> из обычного («синхронно выглядящего») coro-кода.

	coro::await(aw):
		- запускает aw на том же io_context, что и текущий coro::Application;
		- усыпляет текущий фибер (кооперативно) до завершения операции;
		- пробрасывает исключения наружу;
		- поддерживает отмену: если в фибер во время ожидания прилетает
		  исключение (CancelError / TimeoutError), asio-операция отменяется
		  через cancellation_signal — ровно как это делал coro::AsioTask.

	После полного перевода кода на asio-корутины этот файл удаляется.
*/

namespace coro {

namespace detail {

inline std::string bridge_token(const void* tag) {
	return "asio_bridge " + std::to_string(reinterpret_cast<uintptr_t>(tag));
}

} // namespace detail

template <typename T>
T await(asio::awaitable<T> awaitable) {
	Coro* self = Coro::current();
	asio::io_context& ctx = IoService::current()->_impl;

	std::optional<T> result;
	std::exception_ptr error;
	const std::string token = detail::bridge_token(&result);

	asio::cancellation_signal cancel_signal;

	asio::co_spawn(ctx, std::move(awaitable),
		asio::bind_cancellation_slot(cancel_signal.slot(),
			[&](std::exception_ptr e, T value) {
				error = std::move(e);
				if (!error) {
					result.emplace(std::move(value));
				}
				self->resume(token);
			}));

	try {
		self->yield({token, TokenThrow});
	} catch (...) {
		auto pending = std::current_exception();
		cancel_signal.emit(asio::cancellation_type::all);
		self->yield({token});
		std::rethrow_exception(pending);
	}

	if (error) {
		std::rethrow_exception(error);
	}
	return std::move(*result);
}

inline void await(asio::awaitable<void> awaitable) {
	Coro* self = Coro::current();
	asio::io_context& ctx = IoService::current()->_impl;

	std::exception_ptr error;
	const std::string token = detail::bridge_token(&error);

	asio::cancellation_signal cancel_signal;

	asio::co_spawn(ctx, std::move(awaitable),
		asio::bind_cancellation_slot(cancel_signal.slot(),
			[&](std::exception_ptr e) {
				error = std::move(e);
				self->resume(token);
			}));

	try {
		self->yield({token, TokenThrow});
	} catch (...) {
		auto pending = std::current_exception();
		cancel_signal.emit(asio::cancellation_type::all);
		self->yield({token});
		std::rethrow_exception(pending);
	}

	if (error) {
		std::rethrow_exception(error);
	}
}

// Замена coro::Timer: кооперативная пауза на asio::steady_timer.
// Прерывается отменой/таймаутом так же, как любой co_await через мост.
template <typename Duration>
void sleep_for(Duration duration) {
	await([duration]() -> asio::awaitable<void> {
		asio::steady_timer timer(co_await asio::this_coro::executor);
		timer.expires_after(duration);
		co_await timer.async_wait(asio::use_awaitable);
	}());
}

template <typename TimePoint>
void sleep_until(TimePoint time_point) {
	await([time_point]() -> asio::awaitable<void> {
		asio::steady_timer timer(co_await asio::this_coro::executor);
		timer.expires_at(time_point);
		co_await timer.async_wait(asio::use_awaitable);
	}());
}

} // namespace coro
