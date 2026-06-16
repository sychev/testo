
#pragma once

/*!
	@brief asio-only замена бывшей библиотеки 3rd_party/coro.

	Раньше coro был построен на stackful-файберах (ucontext / Win32 Fibers).
	Теперь это тонкий header-only слой поверх C++20-корутин asio (asio::awaitable),
	никаких файберов больше нет — только asio и сопрограммы.

	Каждая операция, которая раньше "блокировала" файбер, теперь возвращает
	asio::awaitable<T> и должна вызываться через co_await.
*/

#include <asio.hpp>

namespace coro {

using executor_type = asio::any_io_executor;

/*!
	@brief Маркер явной отмены.

	Сохранён для совместимости с исходниками, которые ловят coro::CancelError.
	Сама отмена в asio доставляется как asio::error::operation_aborted в точках
	co_await, поэтому отдельный CancelError бросается только при ручной отмене.
*/
struct CancelError {};

namespace detail {

inline executor_type& tls_executor() {
	static thread_local executor_type ex{};
	return ex;
}

inline asio::io_context*& tls_io_context() {
	static thread_local asio::io_context* ctx = nullptr;
	return ctx;
}

} // namespace detail

/// Исполнитель Application, работающего в текущем потоке.
inline executor_type current_executor() {
	return detail::tls_executor();
}

} // namespace coro
