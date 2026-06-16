
#pragma once

#include "coro/IoService.h"
#include "coro/Coro.h"

/*!
	@brief Библиотека для работы с асинхронным вводом/выводом с синхронным кодом
*/
namespace coro {

/*!
	@brief Точка входа в приложение на корутинах

	Создаёт общий io_context, запускает корневую корутину и крутит io_context на @p threads
	потоках. По умолчанию один поток — поведение как у однопоточной модели, существующие
	потребители не меняются. Большее число потоков включает настоящую многопоточность: разные
	корутины выполняются на разных потоках параллельно (каждая на своём strand).

	@code
		coro::Application([&] {
			// здесь можно пользоваться корутинами
		}).run();              // один поток

		coro::Application(main, 16).run();   // 16 потоков
	@endcode
*/
class Application {
public:
	Application(const std::function<void()>& main, unsigned threads = 1);
	~Application();

	/// Запускает приложение и блокирует до завершения всех корутин
	void run();
	/// Отменяет корневую корутину (планирует выброс CancelError) и сразу возвращает управление
	void cancel();

private:
	IoService _ioService;
	std::function<void()> _main;
	unsigned _threads;
	std::shared_ptr<Coro> _root;
};

}
