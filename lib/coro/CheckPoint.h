
#pragma once

#include "coro/Runtime.h"
#include <asio.hpp>

namespace coro {

/*!
	@brief Точка кооперативной отмены.

	Приостанавливает корутину и тут же планирует её возобновление. Если к этому
	моменту корутина была отменена (например, по таймауту), co_await завершится
	с asio::error::operation_aborted, и исключение раскрутит стек — аналог
	прежнего yield с проверкой отмены.
*/
inline asio::awaitable<void> CheckPoint() {
	co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
}

}
