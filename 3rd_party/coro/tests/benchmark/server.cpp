// =============================================================================
//  Бенчмарк coro: СЕРВЕР (бинарник №2)
// -----------------------------------------------------------------------------
//  Назначение
//  ----------
//  Отвечает на сетевые запросы клиента-бенчмарка (см. client.cpp). Это
//  ОДНОПОТОЧНЫЙ асинхронный сервер: один io_context крутится на одном потоке, но
//  благодаря корутинам coro одновременно обслуживает тысячи соединений — пока одно
//  соединение ждёт ввод/вывод, поток занимается другими.
//
//  Протокол (намеренно простой, чтобы мерить именно накладные расходы корутин/IO):
//      клиент  -> сервер:  4 байта  (uint32, little-endian) — идентификатор запроса
//      сервер  -> клиент:  4 байта  (тот же uint32)         — эхо-ответ
//
//  Как это устроено на coro
//  ------------------------
//    Application(main).run()            // один io_context, один рабочий поток
//      └── корневая корутина (main)
//            CoroPool pool;             // структурная конкурентность
//              ├── корутина: вотчер сигналов (SIGINT/SIGTERM) -> аккуратное завершение
//              └── корутина: цикл приёма соединений
//                    Acceptor::run(cb)  // на каждое соединение — отдельная корутина,
//                                       // запущенная во внутреннем CoroPool
//
//  Acceptor::run сам внутри держит CoroPool и на каждое принятое соединение делает
//  pool.exec(handler). Таким образом каждое соединение — это независимая корутина на
//  своём strand; на одном потоке их могут быть тысячи одновременно.
// =============================================================================

#include <coro/Application.h>
#include <coro/Acceptor.h>
#include <coro/StreamSocket.h>
#include <coro/CoroPool.h>
#include <coro/SignalSet.h>

#include <asio.hpp>

#include <array>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

using namespace asio::ip;
using namespace coro;

int main(int argc, char** argv) {
	// Порт можно задать первым аргументом (по умолчанию 5555).
	const uint16_t port = (argc > 1) ? static_cast<uint16_t>(std::atoi(argv[1])) : 5555;

	// Счётчик обслуженных запросов. atomic, потому что в принципе сервер можно было бы
	// запустить и на нескольких потоках; здесь поток один, но так безопаснее и нагляднее.
	std::atomic<uint64_t> handled{0};

	// Сервер однопоточный: Application(main) без второго аргумента => threads == 1.
	Application([&] {
		// Пул, объединяющий вотчер сигналов и цикл приёма.
		CoroPool pool;

		// --- корутина 1: приём и обслуживание соединений ------------------------------
		// Запускаем первой, чтобы получить её дескриптор и затем отменять именно её по сигналу.
		Coro* acceptorCoro = pool.exec([&] {
			// Слушаем на всех интерфейсах указанного порта.
			Acceptor<tcp> acceptor(tcp::endpoint(tcp::v4(), port));
			std::fprintf(stderr, "server: listening on port %u (single-threaded async)\n", port);

			// Acceptor::run в бесконечном цикле принимает соединения и на каждое запускает
			// корутину-обработчик в собственном внутреннем CoroPool. Здесь — тело обработчика
			// одного соединения.
			acceptor.run([&](tcp::socket rawSocket) {
				// Оборачиваем принятый сокет в coro::StreamSocket — он даёт синхронные по
				// виду read/write, которые на самом деле неблокирующе уступают поток.
				StreamSocket<tcp> socket(std::move(rawSocket));

				// Протокол: ровно 4 байта запроса -> ровно 4 байта ответа (эхо).
				std::array<uint8_t, 4> buffer;
				socket.read(asio::buffer(buffer));    // ждём 4 байта (корутина спит, поток свободен)
				socket.write(asio::buffer(buffer));   // отвечаем тем же
				handled.fetch_add(1, std::memory_order_relaxed);
			});
		});

		// --- корутина 2: ожидание сигнала завершения ---------------------------------
		// coro::SignalSet асинхронно ждёт сигнал, не блокируя поток. Получив SIGINT/SIGTERM,
		// отменяем корутину приёма по её дескриптору. Coro::cancel() потокобезопасен (он
		// планирует отмену на strand цели), поэтому вызывать его из другой корутины корректно;
		// accept() при этом прервётся броском CancelError и Acceptor::run аккуратно свернётся.
		pool.exec([&] {
			SignalSet signals({SIGINT, SIGTERM});
			signals.wait();
			std::fprintf(stderr, "server: shutdown signal received\n");
			acceptorCoro->cancel();
		});

		// Ждём завершения обеих корутин (true == не пробрасывать исключения наверх:
		// отмену по сигналу считаем штатным завершением).
		pool.waitAll(true);
	}).run();

	std::fprintf(stderr, "server: done, handled=%llu requests\n",
	             static_cast<unsigned long long>(handled.load()));
	return 0;
}
