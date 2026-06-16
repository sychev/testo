
#include "coro/IoService.h"
#include "coro/CoroPool.h"
#include "coro/AsioTask.h"

namespace coro {

CoroPool::CoroPool():
	_strand(Coro::current()->strand()),
	_allDone(Coro::current()->strand(), 1)
{
}

CoroPool::~CoroPool() {
	cancelAll();
	waitAll(true);
}

Coro* CoroPool::exec(std::function<void()> routine) {
	_running++;

	// Исключение ребёнка кладём в общий "ящик", чтобы прочитать его на strand родителя.
	auto exceptionBox = std::make_shared<std::exception_ptr>();

	auto child = go(IoService::current()->_impl,
		[routine = std::move(routine), exceptionBox] {
			try {
				routine();
			}
			catch (const CancelError&) {
				// отмену в родителя не пробрасываем
			}
			catch (...) {
				*exceptionBox = std::current_exception();
			}
		},
		// onDone выполняется на strand РЕБЁНКА — переносим учёт на strand родителя:
		[this, exceptionBox] {
			asio::post(_strand, [this, exceptionBox] {
				onChildDone(exceptionBox);
			});
		});

	_children.push_back(child);
	return child.get();
}

void CoroPool::onChildDone(std::shared_ptr<std::exception_ptr> exception) {
	_running--;
	if (*exception && !_firstException) {
		_firstException = *exception;
	}
	if (_running == 0 && _waiting) {
		_allDone.try_send(std::error_code{});
	}
}

void CoroPool::waitAll(bool noThrow) {
	if (_running > 0) {
		_waiting = true;
		// Ждём, пока onChildDone не сообщит, что детей не осталось. Если в это время отменят
		// саму родительскую корутину — async_receive вернёт operation_aborted, awaitOp бросит
		// CancelError (и тогда деструктор пула доотменит/дождётся оставшихся детей).
		awaitOp([&](auto&& token) {
			_allDone.async_receive(std::forward<decltype(token)>(token));
		});
		_waiting = false;
	}

	_children.clear();

	if (!noThrow && _firstException) {
		auto exception = _firstException;
		_firstException = nullptr;
		std::rethrow_exception(exception);
	}
}

void CoroPool::cancelAll() {
	for (auto& child: _children) {
		child->cancel();
	}
}

}
