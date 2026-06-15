
#pragma once

#ifdef _MSC_VER
#include "coro/FiberWindows.h"
#endif
#ifdef __GNUC__
#include "coro/FiberLinux.h"
#endif
#include <functional>
#include <list>
#include <exception>

namespace coro {

/*!
	@brief Исключение для отмены корутин

	Специально не наследуется от std::exception, для того чтобы гарантированно полностью раскрутить
	стек корутины. Помни об этом, когда будешь писать catch (...)
*/
struct CancelError {};

/*!
	@brief Токен пробуждения корутины

	Раньше пробуждение различалось по строковым токенам ("Mutex 0x...", "Queue 0x..." и т.п.),
	что приводило к аллокациям и форматированию строк на каждое переключение. Теперь в качестве
	токена используется адрес объекта-инициатора ожидания (this примитива или AsyncOp), а особый
	случай "ожидание только исключения" кодируется нулевым токеном (см. Coro::suspend).
*/
using WaitToken = const void*;

/// Корутина сферическая в вакууме
class Coro {
public:
	static Coro* current();

	explicit Coro(std::function<void()> routine);
	~Coro();

	Coro(const Coro& other) = delete;
	Coro& operator=(const Coro& other) = delete;
	Coro(Coro&& other) = delete;
	Coro& operator=(Coro&& other) = delete;

	/// Запустить корутину (выполняется до первой приостановки или до завершения)
	void start();

	/*!
		@brief Приостановить ТЕКУЩУЮ корутину до пробуждения wake(token)

		Может быть вызвана ТОЛЬКО из той корутины, которая приостанавливается. Вот так:
		@code
			Coro::current()->suspend(this);
		@endcode

		@param token        идентификатор ожидаемого события (обычно this инициатора).
		                    Нулевой token означает "ожидание только инъекции исключения".
		@param interruptible если true, то заброшенное в корутину исключение (cancel /
		                    propagateException) немедленно прервёт ожидание и будет выброшено.
	*/
	void suspend(WaitToken token, bool interruptible = true);

	/*!
		@brief Разбудить корутину, приостановленную на suspend(token)

		Может быть вызвана как извне корутины, так и из другой корутины. Если корутина не
		приостановлена именно на этом token, вызов игнорируется (защита от чужих пробуждений).

		@warning
			Избегайте циклического входа в корутины (coro1 -> coro2 -> coro1). При необходимости
			используйте отложенное пробуждение через IoService::post.
	*/
	void wake(WaitToken token);

	/// Забросить исключение в корутину
	void propagateException(std::exception_ptr exception);
	/// Забросить исключение в корутину
	template <typename Exception>
	void propagateException(Exception exception) {
		propagateException(std::make_exception_ptr(std::move(exception)));
	}
	/// Забросить в корутину исключение CancelError
	void cancel();

	/// Вытащить и перебросить ближайшее запланированное исключение (если есть)
	void rethrowPendingException();

	/// Очередь запланированных исключений
	const std::list<std::exception_ptr>& exceptions() const {
		return _exceptions;
	}

	/// Завершилась ли корутина
	bool done() const {
		return _state == State::Done;
	}

private:
	enum class State { NotStarted, Suspended, Running, Done };

	/// Переключение управления в эту корутину (сохраняя текущую как _previousCoro)
	void switchIn();

	std::function<void()> _routine;
	Fiber _fiber;
	Coro* _previousCoro = nullptr;
	std::list<std::exception_ptr> _exceptions;
	State _state = State::NotStarted;
	WaitToken _waitToken = nullptr;
	bool _interruptible = false;

public:
	void run();
};

}
