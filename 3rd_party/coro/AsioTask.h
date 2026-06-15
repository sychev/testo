
#pragma once

#include <asio.hpp>
#include "coro/Coro.h"
#include <system_error>
#include <tuple>
#include <cassert>

namespace coro {

namespace detail {

/*!
	@brief Ожидание завершения асинхронной операции asio внутри корутины

	Заменяет прежний AsioTask со строковыми токенами и std::function-callback'ами. Операция
	инициируется handler'ом, к которому привязан cancellation_slot — поэтому при инъекции
	исключения в корутину (таймаут / cancel) ожидание отменяется адресной отменой именно этой
	операции (cancellation_type::terminal), а не cancel() всего хэндла.

	AsyncOp размещается на стеке корутины, поэтому handler может безопасно держать на него
	указатель: на пути отмены мы дожидаемся фактического вызова handler'а перед возвратом.
*/
template <typename... Results>
class AsyncOp {
public:
	/// Completion handler для передачи в инициирующую функцию asio
	auto handler() {
		return asio::bind_cancellation_slot(_signal.slot(),
			[this](const std::error_code& errorCode, Results... results) {
				_finished = true;
				_errorCode = errorCode;
				_results = std::tuple<Results...>(std::move(results)...);
				_coro->wake(this);
			});
	}

	/// Дождаться завершения операции, транслируя исключения корутины в отмену операции
	void wait() {
		try {
			_coro->suspend(this, /* interruptible = */ true);
		}
		catch (...) {
			auto exception = std::current_exception();
			_signal.emit(asio::cancellation_type::terminal);
			// Дожидаемся фактического завершения операции, чтобы handler не обратился к уже
			// уничтоженному AsyncOp. На этом ожидании исключения не принимаем.
			_coro->suspend(this, /* interruptible = */ false);
			assert(_finished);
			// не используйте здесь throw, gcc это не переваривает
			std::rethrow_exception(exception);
		}

		if (_errorCode) {
			throw std::system_error(_errorCode);
		}
	}

	template <std::size_t I>
	auto&& result() {
		return std::get<I>(std::move(_results));
	}

private:
	Coro* _coro = Coro::current();
	asio::cancellation_signal _signal;
	std::error_code _errorCode;
	std::tuple<Results...> _results;
	bool _finished = false;
};

} // namespace detail

/*!
	@brief Инициировать асинхронную операцию asio без возвращаемого значения и дождаться её

	@param initiate вызываемый объект, принимающий completion handler и стартующий операцию,
	                handler имеет сигнатуру (const std::error_code&)
*/
template <typename Initiate>
void awaitOp(Initiate&& initiate) {
	detail::AsyncOp<> op;
	initiate(op.handler());
	op.wait();
}

/*!
	@brief Инициировать асинхронную операцию asio с одним возвращаемым значением и дождаться её

	@param initiate вызываемый объект, принимающий completion handler и стартующий операцию,
	                handler имеет сигнатуру (const std::error_code&, Result)
*/
template <typename Result, typename Initiate>
Result awaitValue(Initiate&& initiate) {
	detail::AsyncOp<Result> op;
	initiate(op.handler());
	op.wait();
	return op.template result<0>();
}

}
