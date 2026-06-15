
#include "coro/Coro.h"
#include <cassert>
#include <stdexcept>

#ifdef _DEBUG
#ifndef WIN32
#include <cxxabi.h>
using namespace __cxxabiv1;
#endif
#endif

namespace coro {

thread_local Coro* t_currentCoro = nullptr;

void
#ifdef _MSC_VER
__stdcall
#endif
Run(void* coro) {
	reinterpret_cast<Coro*>(coro)->run();
}

Coro* Coro::current() {
	if (!t_currentCoro) {
		throw std::logic_error("Coro::current is nullptr");
	}
	return t_currentCoro;
}

Coro::Coro(std::function<void()> routine): _routine(std::move(routine)), _fiber(Run, this)
{
}

Coro::~Coro() {
	assert(_state == State::NotStarted || _state == State::Done);
#ifdef _DEBUG
	std::string what;
	for (auto exception: _exceptions) {
		try {
			std::rethrow_exception(exception);
		}
		catch (const std::exception& error) {
			what += error.what();
			what += "\n";
		}
		catch (const CancelError&) {
			continue;
		}
		catch (...) {
#ifndef WIN32
			what += abi::__cxa_current_exception_type()->name();
#else
			what += "Unknown Exception Type";
#endif
			what += "\n";
		}
	}
	if (what.size()) {
		printf("Coro::~Coro: unhandled exceptions:\n%s", what.c_str());
	}
#endif
}

void Coro::switchIn() {
	_previousCoro = t_currentCoro;
	t_currentCoro = this;
	if (_previousCoro) {
		_previousCoro->_fiber.switchTo(_fiber);
	} else {
		_fiber.enter();
	}
	t_currentCoro = _previousCoro;
	_previousCoro = nullptr;
}

void Coro::start() {
	assert(_state == State::NotStarted);
	switchIn();
}

void Coro::wake(WaitToken token) {
	// Нулевой токен зарезервирован под "ожидание только исключения" и не пробуждается wake().
	if (token == nullptr) {
		return;
	}
	if (_state == State::Suspended && _waitToken == token) {
		switchIn();
	}
}

void Coro::suspend(WaitToken token, bool interruptible) {
	if (interruptible) {
		rethrowPendingException();
	}

	_waitToken = token;
	_interruptible = interruptible;
	_state = State::Suspended;

	if (_previousCoro) {
		_fiber.switchTo(_previousCoro->_fiber);
	} else {
		_fiber.exit();
	}

	_state = State::Running;
	_waitToken = nullptr;
	_interruptible = false;

	if (interruptible) {
		rethrowPendingException();
	}
}

void Coro::propagateException(std::exception_ptr exception) {
	assert(exception);
	_exceptions.push_back(exception);

	// Если корутина прямо сейчас приостановлена и готова принять исключение — будим её, чтобы
	// исключение было выброшено немедленно. Иначе оно дождётся ближайшей прерываемой приостановки.
	if (_state == State::Suspended && _interruptible) {
		switchIn();
	}
}

void Coro::cancel() {
	propagateException(CancelError());
}

void Coro::rethrowPendingException() {
	if (_exceptions.size()) {
		auto exception = _exceptions.front();
		_exceptions.pop_front();
		assert(exception);
		std::rethrow_exception(exception);
	}
}

void Coro::run() {
	_state = State::Running;
	try
	{
		_routine();
	}
	catch (const CancelError&) {
		// do nothing
	}
	catch (...)
	{
		auto exception = std::current_exception();
		assert(exception);
		_exceptions.push_front(exception);
	}
	_routine = nullptr;
	_state = State::Done;

	// Финальное переключение обратно к инициатору. Сюда мы больше не вернёмся.
	if (_previousCoro) {
		_fiber.switchTo(_previousCoro->_fiber);
	} else {
		_fiber.exit();
	}
}

}
