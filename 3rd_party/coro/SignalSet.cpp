
#include "coro/SignalSet.h"
#include "coro/IoService.h"

namespace coro {

SignalSet::SignalSet(const std::initializer_list<int32_t>& signals)
	: _handle(IoService::current()->_impl)
{
	for (auto signal: signals) {
		_handle.add(signal);
	}
}

int32_t SignalSet::wait() {
	return awaitValue<int>([&](auto&& handler) {
		_handle.async_wait(std::forward<decltype(handler)>(handler));
	});
}

}
