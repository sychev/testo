
#include "coro/IoService.h"
#include "coro/CheckPoint.h"
#include "coro/Coro.h"

namespace coro {

void CheckPoint() {
	auto coro = Coro::current();
	IoService::current()->post([coro] {
		IoService::current()->checkpoints.push([coro] {
			coro->wake(coro);
		});
	});
	// Токеном выступает адрес самой корутины: в каждый момент времени активна не более одной
	// контрольной точки на корутину.
	coro->suspend(coro);
}

}
