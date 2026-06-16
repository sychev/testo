
#pragma once

#include <asio/steady_timer.hpp>
#include "coro/Coro.h"
#include <stdexcept>

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
	@brief Таймаут на блокирующий (по виду) вызов корутины

	Ставит таймер на strand текущей корутины. По срабатыванию просит корутину отмениться,
	помечая отмену как таймаут — ближайшая ожидаемая операция корутины завершится броском
	TimeoutError (см. detail::checkAbort в AsioTask.h).

	Так как таймер живёт на strand корутины, его обработчик сериализован с самой корутиной:
	гонок между «корутина выполняется» и «таймер сработал» нет.

	@warning Этот код НЕ РАБОТАЕТ (это объявление функции, а не переменной):
	@code
		Timeout timeout(std::chrono::seconds(TIMEOUT));
	@endcode
*/
class Timeout {
public:
	template <typename Duration>
	Timeout(Duration duration): _timer(Coro::current()->strand()), _coro(Coro::current()) {
		_timer.expires_after(duration);
		_timer.async_wait([this](const std::error_code& errorCode) {
			if (errorCode) {
				return;   // таймер отменён в деструкторе — таймаут не наступил
			}
			_coro->requestTimeout(this);
		});
	}

	~Timeout() {
		// Отменяем таймер. Поскольку и деструктор, и обработчик таймера выполняются на одном
		// strand, они не пересекаются: если обработчик ещё не вызван, cancel() его снимет.
		_timer.cancel();
	}

private:
	asio::steady_timer _timer;
	Coro* _coro;
};

}
