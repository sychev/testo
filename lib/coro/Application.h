
#pragma once

#include "coro/Runtime.h"
#include <asio.hpp>
#include <exception>
#include <functional>
#include <utility>

namespace coro {

/*!
	@brief Точка входа в приложение на C++20-корутинах asio.

	Пример:
	@code
		int main() {
			coro::Application([&]() -> asio::awaitable<void> {
				// здесь можно пользоваться co_await
				co_return;
			}).run();
		}
	@endcode
*/
class Application {
public:
	using main_type = std::function<asio::awaitable<void>()>;

	explicit Application(main_type main): _main(std::move(main)) {}

	Application(const Application&) = delete;
	Application& operator=(const Application&) = delete;

	/// Запускает приложение в текущем потоке до завершения корневой корутины.
	void run() {
		detail::tls_io_context() = &_ctx;
		detail::tls_executor() = _ctx.get_executor();

		std::exception_ptr eptr;
		asio::co_spawn(_ctx,
			[this]() -> asio::awaitable<void> {
				co_await _main();
			},
			asio::bind_cancellation_slot(_cancel.slot(),
				[&eptr](std::exception_ptr e) {
					eptr = e;
				}));

		_ctx.run();

		detail::tls_io_context() = nullptr;
		detail::tls_executor() = executor_type{};

		if (eptr) {
			try {
				std::rethrow_exception(eptr);
			} catch (const CancelError&) {
				// корневая корутина была отменена — это штатное завершение
			} catch (const asio::system_error& e) {
				if (e.code() != asio::error::operation_aborted) {
					throw;
				}
			}
		}
	}

	/// Планирует отмену корневой корутины и сразу возвращает управление.
	void cancel() {
		asio::post(_ctx, [this] {
			_cancel.emit(asio::cancellation_type::all);
		});
	}

private:
	asio::io_context _ctx;
	main_type _main;
	asio::cancellation_signal _cancel;
};

}
