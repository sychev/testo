
#include "coro/IoService.h"
#include "coro/CoroPool.h"
#include <coro/Finally.h>
#include <cassert>

namespace coro {

CoroPool::~CoroPool() {
	cancelAll();
	waitAll(true);
}

CoroPool::CoroPool(CoroPool&& other):
	_parentCoro(std::move(other._parentCoro)),
	_childCoros(std::move(other._childCoros))
{
	other._parentCoro = nullptr;
}

CoroPool& CoroPool::operator=(CoroPool&& other) {
	std::swap(_parentCoro, other._parentCoro);
	std::swap(_childCoros, other._childCoros);
	return *this;
}

Coro* CoroPool::exec(std::function<void()> routine) {
	auto coro = new Coro([=] {
		Finally cleanup([=] {
			onCoroDone(Coro::current());
		});

		routine();
	});
	_childCoros.insert(coro);
	coro->start();
	return coro;
}

void CoroPool::waitAll(bool noThrow) {
	if (_childCoros.empty()) {
		return;
	}

	// noThrow == true  -> ждём без возможности прерывания (как прежний {token()})
	// noThrow == false -> ждём прерываемо (как прежний {token(), TokenThrow})
	_parentCoro->suspend(this, /* interruptible = */ !noThrow);

	assert(_childCoros.empty());
}

void CoroPool::cancelAll() {
	for (auto coro: _childCoros) {
		if (_childCoros.find(coro) != _childCoros.end()) {
			coro->propagateException(CancelError());
		}
	}
}

void CoroPool::onCoroDone(Coro* childCoro) {
	IoService::current()->post([=] {
		while (childCoro->exceptions().size()) {
			try {
				childCoro->rethrowPendingException();
			}
			catch (const CancelError&) {
				// CancelError не пробрасываем в родительскую корутину
				// (пытаться отменить корутину можно сколько угодно раз, поэтому исключений может быть несколько)
			}
			catch (...) {
				_parentCoro->propagateException(std::current_exception());
			}
		}
		_childCoros.erase(childCoro);
		delete childCoro;

		if (_childCoros.empty()) {
			_parentCoro->wake(this);
		}
	});
}

}
