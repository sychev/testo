
#pragma once

#include <asio/steady_timer.hpp>
#include "coro/Coro.h"
#include "coro/IoService.h"

namespace coro {

class Timeout;

/// Исключение, выбрасываемое при срабатывании таймаута
class TimeoutError: public std::runtime_error {
public:
	TimeoutError(Timeout* timeout): std::runtime_error("Timeout was triggered"), _timeout(timeout) {}

	Timeout* timeout() const {
		return _timeout;
	}

private:
	Timeout* _timeout;
};

/*!
	@brief Таймаут, что ещё тут скажешь

	@warning Этот код НЕ РАБОТАЕТ:
	@code
		class A {
		public:
			enum { TIMEOUT = 10 };

			void f() {
				Timeout timeout(std::chrono::seconds(TIMEOUT));
				....
			}
		};
	@endcode
	Здесь не объявление переменной, здесь объявление ФУНКЦИИ
*/
class Timeout {
public:
	/// Установить таймаут
	template <typename Duration>
	Timeout(Duration duration): _timer(IoService::current()->_impl) {
		_timer.expires_after(duration);
		_timer.async_wait([this](const std::error_code& errorCode) {
			_callbackExecuted = true;
			if (_timerCanceled) {
				return _coro->wake(this);
			}
			if (errorCode) {
				return _coro->propagateException(std::system_error(errorCode));
			}
			_coro->propagateException(TimeoutError(this));
		});
	}
	/// Снять таймаут
	~Timeout() {
		if (!_callbackExecuted) {
			_timerCanceled = true;
			_timer.cancel();
			// Дожидаемся фактического вызова callback'а таймера (operation_aborted),
			// чтобы он не обратился к уже уничтоженному Timeout. Прерывания не принимаем.
			_coro->suspend(this, /* interruptible = */ false);
		}
	}

private:
	asio::steady_timer _timer;
	Coro* _coro = Coro::current();
	bool _timerCanceled = false, _callbackExecuted = false;
};

}
