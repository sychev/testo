
#include "coro/Application.h"
#include <thread>
#include <vector>

namespace coro {

Application::Application(const std::function<void()>& main, unsigned threads):
	_main(main), _threads(threads ? threads : 1)
{
	// Выставляем общий io_context до старта рабочих потоков.
	IoService::setCurrent(&_ioService);
}

Application::~Application() {
	IoService::setCurrent(nullptr);
}

void Application::run() {
	// work guard держит io_context живым, пока не завершится корневая корутина — чтобы
	// рабочие потоки не вышли из run() раньше времени при кратковременном отсутствии работы.
	auto work = asio::make_work_guard(_ioService._impl);

	_root = go(_ioService._impl, _main, [&work] {
		work.reset();   // корень завершился — отпускаем io_context
	});

	if (_threads == 1) {
		_ioService._impl.run();
	} else {
		std::vector<std::thread> pool;
		pool.reserve(_threads - 1);
		for (unsigned i = 1; i < _threads; ++i) {
			pool.emplace_back([this] { _ioService._impl.run(); });
		}
		_ioService._impl.run();
		for (auto& thread: pool) {
			thread.join();
		}
	}
}

void Application::cancel() {
	if (_root) {
		_root->cancel();
	}
}

}
