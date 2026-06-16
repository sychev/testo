
#pragma once

#include <asio.hpp>
#include <asio/experimental/channel_error.hpp>
#include "coro/Coro.h"
#include "coro/Timeout.h"
#include <system_error>

namespace coro {

namespace detail {

/*!
	@brief Разобрать результат завершённой async-операции

	operation_aborted означает, что операцию отменили снаружи: либо это запрошенная отмена
	корутины (CancelError), либо сработавший Timeout (TimeoutError). Остальные ошибки
	пробрасываются как system_error.
*/
inline void checkAbort(Coro* self, const std::error_code& errorCode) {
	// Отмену по cancellation_slot socket/timer-операции asio сообщает как operation_aborted,
	// а операции каналов (Queue/Mutex) — как channel_cancelled. Оба случая — это отмена корутины.
	if (errorCode == asio::error::operation_aborted ||
	    errorCode == asio::experimental::error::channel_cancelled) {
		if (Timeout* timeout = self->takePendingTimeout()) {
			throw TimeoutError(timeout);
		}
		throw CancelError{};
	}
	if (errorCode) {
		throw std::system_error(errorCode);
	}
}

} // namespace detail

/*!
	@brief Инициировать async-операцию asio без возвращаемого значения и дождаться её

	Операция выполняется через yield_context текущей корутины (поэтому её завершение
	сериализуется на strand корутины). После возобновления — возможно на ДРУГОМ потоке —
	восстанавливаем thread_local "текущая корутина"; это и позволяет примитивам узнавать
	текущую корутину, не таская yield в каждой сигнатуре (невирусный публичный API).

	@param initiate вызываемый объект, принимающий completion token и стартующий операцию.
*/
template <typename Initiate>
void awaitOp(Initiate&& initiate) {
	Coro* self = Coro::current();
	std::error_code errorCode;
	initiate(self->yield()[errorCode]);
	Coro::setCurrent(self);
	detail::checkAbort(self, errorCode);
}

/*!
	@brief Инициировать async-операцию asio с возвращаемым значением и дождаться её

	@param initiate вызываемый объект, принимающий completion token, стартующий операцию
	                и ВОЗВРАЩАЮЩИЙ её результат.
*/
template <typename Result, typename Initiate>
Result awaitValue(Initiate&& initiate) {
	Coro* self = Coro::current();
	std::error_code errorCode;
	Result result = initiate(self->yield()[errorCode]);
	Coro::setCurrent(self);
	detail::checkAbort(self, errorCode);
	return result;
}

}
