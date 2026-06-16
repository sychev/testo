// =============================================================================
//  Бенчмарк coro: КЛИЕНТ / прототип приложения (бинарник №1)
// -----------------------------------------------------------------------------
//  Назначение
//  ----------
//  Демонстрирует, что библиотека coro масштабируется как «серьёзный» рантайм:
//  запускает 1000 корутин на 16 потоках поверх ОДНОГО общего io_context, повторяя
//  по структуре вызовов главное приложение testo, но без тяжёлых операций (VM и т.п.).
//  Сетевую часть (TCP-запросы) обслуживает второй бинарник — server.cpp.
//
//  Иерархия вызовов (как в testo: Application -> CoroPool -> задачи -> под-задачи)
//  -----------------------------------------------------------------------------
//    Application(main, threads=16).run()        // общий io_context на 16 потоках
//      └── корневая корутина (main)
//            CoroPool jobs;                      // верхний пул "задач" (аналог пула
//              │                                 //  тестов в testo)
//              ├── задача 0
//              │     CoroPool sub;               // вложенный пул "подготовки"
//              │       ├── под-корутина (async-ожидание)
//              │       └── под-корутина (async-ожидание)
//              │     sub.waitAll();
//              │     Timeout + TCP запрос к серверу (connect/write/read)
//              ├── задача 1
//              ...
//              └── задача N-1
//            jobs.waitAll();
//
//  Почему это работает на 16 потоках
//  ---------------------------------
//  В MT-модели каждая корутина запускается на собственном asio::strand. Strand
//  гарантирует, что одна корутина не выполняется на двух потоках сразу, но РАЗНЫЕ
//  корутины свободно раскладываются по 16 потокам общего io_context. Поэтому 1000
//  задач реально исполняются параллельно, а пока каждая ждёт сеть — поток занят другими.
//  Никакого разделяемого между потоками изменяемого состояния здесь нет (счётчики —
//  atomic), поэтому всё потокобезопасно по построению.
//
//  Параметры командной строки (все необязательные):
//      argv[1] = число корутин-задач   (по умолчанию 1000)
//      argv[2] = число потоков         (по умолчанию 16)
//      argv[3] = порт сервера          (по умолчанию 5555)
//      argv[4] = хост сервера          (по умолчанию 127.0.0.1)
// =============================================================================

#include <coro/Application.h>
#include <coro/CoroPool.h>
#include <coro/StreamSocket.h>
#include <coro/Timer.h>
#include <coro/Timeout.h>

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace asio::ip;
using namespace coro;

int main(int argc, char** argv) {
	const unsigned coros   = (argc > 1) ? static_cast<unsigned>(std::atoi(argv[1])) : 1000;
	const unsigned threads = (argc > 2) ? static_cast<unsigned>(std::atoi(argv[2])) : 16;
	const uint16_t port    = (argc > 3) ? static_cast<uint16_t>(std::atoi(argv[3])) : 5555;
	const std::string host = (argc > 4) ? argv[4] : "127.0.0.1";

	const auto endpoint = tcp::endpoint(make_address(host), port);

	// Метрики. atomic, потому что задачи выполняются на 16 потоках одновременно.
	std::atomic<uint64_t> ok{0};
	std::atomic<uint64_t> failed{0};

	const auto started = std::chrono::steady_clock::now();

	// threads == 16 включает настоящую многопоточность общего io_context.
	Application([&] {
		// Верхний пул задач. Его деструктор/waitAll дождётся всех 1000 корутин.
		CoroPool jobs;

		for (unsigned i = 0; i < coros; ++i) {
			// Каждая задача — отдельная корутина на своём strand. Запускаются они
			// неблокирующе (exec возвращает управление сразу), а реально исполняются
			// параллельно по 16 потокам по мере готовности.
			jobs.exec([&, i] {
				try {
					// (1) Вложенная иерархия, как «под-шаги» теста в testo: небольшой
					//     CoroPool из двух корутин, делающих асинхронную работу. Здесь это
					//     просто короткие таймеры — имитация подготовительных операций без
					//     тяжёлой логики.
					{
						CoroPool sub;
						sub.exec([] { Timer t; t.waitFor(std::chrono::milliseconds(1)); });
						sub.exec([] { Timer t; t.waitFor(std::chrono::milliseconds(1)); });
						sub.waitAll();
					}

					// (2) Сетевой запрос к серверу под защитой таймаута. Timeout ставит
					//     таймер на strand этой корутины; если сеть зависнет, ближайшая
					//     операция (connect/read/write) будет прервана броском TimeoutError.
					Timeout timeout(std::chrono::seconds(5));

					StreamSocket<tcp> socket;
					socket.connect(endpoint);

					const uint32_t request = i;
					socket.write(asio::buffer(&request, sizeof(request)));

					uint32_t response = 0;
					socket.read(asio::buffer(&response, sizeof(response)));

					if (response == request) {
						ok.fetch_add(1, std::memory_order_relaxed);
					} else {
						failed.fetch_add(1, std::memory_order_relaxed);
					}
				}
				catch (...) {
					// Любая ошибка (таймаут, отказ соединения, и т.п.) — это неуспех задачи.
					failed.fetch_add(1, std::memory_order_relaxed);
				}
			});
		}

		// Ждём завершения всех задач. true == не пробрасывать исключения наверх: каждая
		// задача уже сама обработала свои ошибки и отразила их в счётчике failed.
		jobs.waitAll(true);
	}, threads).run();

	const auto finished = std::chrono::steady_clock::now();
	const double seconds = std::chrono::duration<double>(finished - started).count();

	std::fprintf(stderr,
	             "client: coros=%u threads=%u ok=%llu failed=%llu time=%.3fs rps=%.0f\n",
	             coros, threads,
	             static_cast<unsigned long long>(ok.load()),
	             static_cast<unsigned long long>(failed.load()),
	             seconds, seconds > 0 ? ok.load() / seconds : 0.0);

	return (failed.load() == 0) ? 0 : 1;
}
