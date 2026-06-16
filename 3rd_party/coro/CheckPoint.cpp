
#include "coro/CheckPoint.h"
#include "coro/Coro.h"
#include "coro/AsioTask.h"
#include <asio/steady_timer.hpp>

namespace coro {

void CheckPoint() {
	// Уступаем управление циклу событий: ставим таймер с нулевой задержкой на strand корутины
	// и ждём его. Это даёт остальным готовым задачам шанс выполниться. Точка прерываемая —
	// отмена/таймаут корутины бросят CancelError/TimeoutError.
	Coro* self = Coro::current();
	asio::steady_timer timer(self->strand());
	timer.expires_after(std::chrono::seconds(0));
	awaitOp([&](auto&& token) {
		timer.async_wait(std::forward<decltype(token)>(token));
	});
}

}
