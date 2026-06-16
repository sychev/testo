
#include "coro/IoService.h"

namespace coro {

// Единый io_context на все потоки. Устанавливается в конструкторе Application (до старта
// рабочих потоков) и далее только читается.
static IoService* g_current = nullptr;

IoService* IoService::current() {
	if (!g_current) {
		throw std::runtime_error("IoService::current() is nullptr");
	}
	return g_current;
}

void IoService::setCurrent(IoService* ioService) {
	g_current = ioService;
}

}
